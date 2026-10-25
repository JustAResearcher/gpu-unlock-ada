/* SPDX-License-Identifier: GPL-2.0-only */
#include "staged_state.h"
#include "app_io.h"
#include "gfw_eval.h"

typedef struct {
  const char *Name;
  UINT32 Offset, Value;
  BOOLEAN Permission;
} GU_POST_REGISTER;

static BOOLEAN Inaccessible(UINT32 Value) {
  return Value == 0xffffffffU || (Value & 0xffff0000U) == 0xbadf0000U;
}

static EFI_STATUS TransportStatus(EFI_STATUS Status) {
  /* EFI warnings are not a successful observation, even when a callback fills
   * its output. The generic port helpers historically accept warnings. */
  if (Status != EFI_SUCCESS) {
    Gu.fault = TRUE;
    return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;
  }
  return Gu.fault ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

static EFI_STATUS ReadConfig(EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset,
                             VOID *Value) {
  if (Gu.fault) return EFI_DEVICE_ERROR;
  return TransportStatus(Gu.pci->Pci.Read(Gu.pci, Width, Offset, 1, Value));
}

static EFI_STATUS ReadRegister(UINT32 Offset, UINT32 *Value) {
  if (Gu.fault) return EFI_DEVICE_ERROR;
  if ((Offset & 3U) || (UINT64)Offset + 4U > Gu.bar_size) {
    Gu.fault = TRUE;
    return EFI_INVALID_PARAMETER;
  }
  return TransportStatus(Gu.pci->Mem.Read(Gu.pci, EfiPciIoWidthUint32,
                                         0, Offset, 1, Value));
}

static BOOLEAN Lifecycle(VOID) {
  return Gu.started && Gu.quiesced && Gu.saved && Gu.flrs == 1 &&
    Gu.mmio_writes != 0 && !Gu.fault && !Gu.retained &&
    !Gu.master_attributes_pending && Gu.dma_count == 0 &&
    Gu.allocations == NULL && Gu.master_attribute_status == EFI_SUCCESS &&
    Gu.master_hardware_status == EFI_SUCCESS;
}

static EFI_STATUS Identity(UINT32 *Chip) {
  EFI_STATUS Status;
  UINT32 Id = 0, Subsystem = 0;
  UINT16 Command = 0, Pending = 0;
  Status = ReadConfig(EfiPciIoWidthUint32, 0, &Id);
  if (Status != EFI_SUCCESS) return Status;
  Status = ReadConfig(EfiPciIoWidthUint32, 0x2c, &Subsystem);
  if (Status != EFI_SUCCESS) return Status;
  Status = ReadConfig(EfiPciIoWidthUint16, 4, &Command);
  if (Status != EFI_SUCCESS) return Status;
  Status = ReadConfig(EfiPciIoWidthUint16, Gu.pcie_cap + 10U, &Pending);
  if (Status != EFI_SUCCESS) return Status;
  if (Id != GU_BOARD_ID || Subsystem != GU_BOARD_SUBSYSTEM ||
      Command == 0xffffU || !(Command & GU_PCI_COMMAND_MEMORY) ||
      (Command & GU_PCI_COMMAND_MASTER) || Pending == 0xffffU ||
      (Pending & 0x20U)) return EFI_NOT_READY;
  Status = ReadRegister(0, Chip);
  if (Status != EFI_SUCCESS) return Status;
  if (Inaccessible(*Chip) || *Chip != GU_BOARD_CHIP) return EFI_NOT_READY;
  return EFI_SUCCESS;
}

EFI_STATUS GuStagedPostState(BOOLEAN Log) {
  /* These are the core's post-FLR conditions, not its cold-entry conditions.
   * WPR_CFG/WPR permission changes may be reset by FLR: the repository's 4090
   * evidence demonstrates that. Sample all four permission registers, as the
   * core does, but require only FEAT to retain its unlocked value after FLR.
   * All-ones is valid for a PLM only with independent identity before/after. */
  enum { Vram, WprCfg, Fbpa, Wpr, Feat, Ss0, Ss1, Readout,
         GfwPlm, GfwBoot, GfwCpu, WprLow, WprHigh, RegisterCount };
  GU_POST_REGISTER Registers[RegisterCount] = {
    {"VRAM", 0x001183a4U, 0, FALSE},
    {"WPR_CFG", 0x001fa7ccU, 0, TRUE},
    {"FBPA", 0x009a0148U, 0, TRUE},
    {"WPR", 0x001fa7c4U, 0, TRUE},
    {"FEAT", GU_FEAT_PLM_OFFSET, 0, TRUE},
    {"SS0", GU_FEAT_SS0_OFFSET, 0, FALSE},
    {"SS1", GU_FEAT_SS1_OFFSET, 0, FALSE},
    {"READOUT1", GU_FEAT_READOUT_1_OFFSET, 0, FALSE},
    {"GFW_PLM", GU_GFW_PLM_OFFSET, 0, FALSE},
    {"GFW_BOOT", GU_GFW_BOOT_OFFSET, 0, FALSE},
    {"GFW_CPUCTL", GU_GFW_CPUCTL_OFFSET, 0, FALSE},
    {"WPR2_LO", 0x001fa824U, 0, FALSE},
    {"WPR2_HI", 0x001fa828U, 0, FALSE},
  };
  EFI_STATUS Status = EFI_NOT_READY;
  const char *Reason = "lifecycle";
  UINT32 Chip = 0;
  UINTN I;

  if (Log && !GuLogReady()) return EFI_DEVICE_ERROR;
  if (!Lifecycle()) goto Done;
  Reason = "interface-profile";
  if (!Gu.pci || !Gu.pci->Pci.Read || !Gu.pci->Mem.Read ||
      !GU_BOARD_CHIP || !GU_PROFILE_VRAM || !GU_PROFILE_BAR_BASE ||
      Gu.bar_base != GU_PROFILE_BAR_BASE || Gu.bar_size != GU_PROFILE_BAR_SIZE ||
      Gu.pcie_cap < 0x40U || Gu.pcie_cap > 0xd4U || (Gu.pcie_cap & 3U))
    goto Done;

  Reason = "identity-before";
  Status = Identity(&Chip);
  if (Status != EFI_SUCCESS) goto Done;
  for (I = 0; I < RegisterCount; I++) {
    GU_POST_REGISTER *Register = &Registers[I];
    Reason = Register->Name;
    Status = ReadRegister(Register->Offset, &Register->Value);
    if (Status != EFI_SUCCESS) goto Done;
    if (Log) {
      if (!GuLogReady()) { Status = EFI_DEVICE_ERROR; goto Done; }
      GuLog("STAGED_POST_REGISTER name=%s offset=%#x value=%#x\n",
            Register->Name, Register->Offset, Register->Value);
      if (!GuLogReady()) { Status = EFI_DEVICE_ERROR; goto Done; }
    }
    if ((Register->Value & 0xffff0000U) == 0xbadf0000U ||
        (!Register->Permission && Inaccessible(Register->Value))) {
      Status = EFI_DEVICE_ERROR;
      goto Done;
    }
  }
  Reason = "identity-after";
  Status = Identity(&Chip);
  if (Status != EFI_SUCCESS) goto Done;
  Status = EFI_NOT_READY;
  Reason = "vram";
  if (Registers[Vram].Value != GU_PROFILE_VRAM) goto Done;
  Reason = "selectors";
  if (Registers[Ss0].Value != 0x88888888U || Registers[Ss1].Value != 8U)
    goto Done;
  Reason = "feat-permission";
  if (Registers[Feat].Value != 0xffffffffU) goto Done;
  Reason = "effective-readout";
  if (Registers[Readout].Value & 0x07ffffffU) goto Done;
  Reason = "gfw";
  if (!(Registers[GfwPlm].Value & GU_GFW_PLM_LEVEL0) ||
      (Registers[GfwBoot].Value & 0xffU) != GU_GFW_BOOT_COMPLETED ||
      !(Registers[GfwCpu].Value & GU_GFW_CPUCTL_HALTED)) goto Done;
  Reason = "wpr2-enabled";
  if (Registers[WprHigh].Value >> 4U) goto Done;
  Reason = "lifecycle-after";
  if (!Lifecycle()) goto Done;
  Reason = "verified";
  Status = EFI_SUCCESS;
Done:
  if (Log) {
    if (!GuLogReady()) return EFI_DEVICE_ERROR;
    GuLog("STAGED_POST_STATE reason=%s status=%#llx chip=%#x\n", Reason,
          (unsigned long long)Status, Chip);
    if (!GuLogReady()) return EFI_DEVICE_ERROR;
  }
  return Status;
}
