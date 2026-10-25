/* SPDX-License-Identifier: GPL-2.0-only */
#include "staged_state.h"
#include "gfw_eval.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GU_PLATFORM Gu;
static EFI_PCI_IO_PROTOCOL Pci;
static GU_DMA Allocation;
static UINT8 Config[256];
static struct {UINT32 Offset, Value;} Registers[] = {
  {0, 0x193000a1U}, {0x001183a4U, 16376U},
  {0x001fa7ccU, 0x4cb8fU}, {0x009a0148U, 0xffffff0fU},
  {0x001fa7c4U, 0x4cb8fU}, {GU_FEAT_PLM_OFFSET, 0xffffffffU},
  {GU_FEAT_SS0_OFFSET, 0x88888888U}, {GU_FEAT_SS1_OFFSET, 8U},
  {GU_FEAT_READOUT_1_OFFSET, 0}, {GU_GFW_PLM_OFFSET, 1U},
  {GU_GFW_BOOT_OFFSET, 0xffU}, {GU_GFW_CPUCTL_OFFSET, 0x10U},
  {0x001fa824U, 0}, {0x001fa828U, 0},
};
static UINTN Reads, ConfigReads, MmioReads, Logs, LogQueries, Forbidden;
static UINTN FailRead, FailLog, CloseLogAtRead;
static EFI_STATUS InjectedStatus;
static BOOLEAN LogReady, LateIdentity, LateCommand, LatePending, LateChip;
static BOOLEAN LateLifecycle, CallbackFault;
static const char *Scenario;
static BOOLEAN Is(const char *Name) {return !strcmp(Scenario, Name);}

static EFI_STATUS ReadStatus(VOID) {
  Reads++;
  if (Reads == CloseLogAtRead) LogReady = FALSE;
  if (CallbackFault && Reads == 3) Gu.fault = TRUE;
  if (LateLifecycle && Reads == 18) Gu.dma_count = 1;
  if (Reads == FailRead) return InjectedStatus;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI ConfigRead(EFI_PCI_IO_PROTOCOL *Io,
  EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, UINTN Count, VOID *Out) {
  UINTN Bytes = Width == EfiPciIoWidthUint32 ? 4 : 2;
  assert(Io == &Pci && Out && Count == 1);
  assert(Width == EfiPciIoWidthUint32 || Width == EfiPciIoWidthUint16);
  assert(Offset + Bytes <= sizeof(Config));
  ConfigReads++;
  memcpy(Out, Config + Offset, Bytes);
  if (Reads >= 18) {
    if (LateIdentity && Offset == 0) *(UINT32 *)Out = 0xffffffffU;
    if (LateCommand && Offset == 4) *(UINT16 *)Out |= 4U;
    if (LatePending && Offset == 0x7aU) *(UINT16 *)Out |= 0x20U;
  }
  return ReadStatus();
}

static EFI_STATUS EFIAPI MemRead(EFI_PCI_IO_PROTOCOL *Io,
  EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT8 Bar, UINT64 Offset,
  UINTN Count, VOID *Out) {
  UINTN I;
  assert(Io == &Pci && Out && Width == EfiPciIoWidthUint32 && Bar == 0 && Count == 1);
  assert(!(Offset & 3U) && Offset + 4 <= Gu.bar_size);
  MmioReads++;
  for (I = 0; I < sizeof(Registers) / sizeof(Registers[0]); I++) {
    if (Registers[I].Offset != Offset) continue;
    *(UINT32 *)Out = LateChip && Reads >= 18 && Offset == 0
      ? 0xffffffffU : Registers[I].Value;
    return ReadStatus();
  }
  assert(!"unexpected MMIO register");
  return EFI_DEVICE_ERROR;
}

static EFI_STATUS EFIAPI ConfigWrite(EFI_PCI_IO_PROTOCOL *Io,
  EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, UINTN Count, VOID *Value) {
  (void)Io; (void)Width; (void)Offset; (void)Count; (void)Value;
  Forbidden++; return EFI_DEVICE_ERROR;
}

static EFI_STATUS EFIAPI MemWrite(EFI_PCI_IO_PROTOCOL *Io,
  EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT8 Bar, UINT64 Offset,
  UINTN Count, VOID *Value) {
  (void)Io; (void)Width; (void)Bar; (void)Offset; (void)Count; (void)Value;
  Forbidden++; return EFI_DEVICE_ERROR;
}

BOOLEAN GuLogReady(VOID) {LogQueries++; return LogReady;}
VOID GuLog(const char *Format, ...) {
  va_list Args;
  assert(LogReady);
  Logs++;
  va_start(Args, Format); vfprintf(stderr, Format, Args); va_end(Args);
  if (Logs == FailLog) LogReady = FALSE;
}
/* No fallback through port helpers, console, allocation, or ownership APIs. */
VOID GuConsole(const char *Text) {(void)Text; Forbidden++;}
VOID *GuAlloc(size_t Bytes) {(void)Bytes; Forbidden++; return NULL;}
VOID GuFree(VOID *Memory) {(void)Memory; Forbidden++;}
UINT32 GuRead32(UINT32 Offset) {(void)Offset; Forbidden++; return 0xffffffffU;}
VOID GuWrite32(UINT32 Offset, UINT32 Value) {(void)Offset; (void)Value; Forbidden++;}
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, VOID *Out) {
  (void)Width; (void)Offset; (void)Out; Forbidden++; return EFI_DEVICE_ERROR;
}

static VOID SetWord(UINTN Offset, UINT16 Value) {memcpy(Config + Offset, &Value, 2);}
static VOID SetDword(UINTN Offset, UINT32 Value) {memcpy(Config + Offset, &Value, 4);}

int main(int Argc, char **Argv) {
  EFI_STATUS Status;
  BOOLEAN Success = FALSE, Silent = FALSE, ExpectFault = FALSE, NoReads = FALSE;
  UINT8 BeforeConfig[sizeof(Config)];
  UINT8 BeforeRegisters[sizeof(Registers)];
  unsigned Index, Value;
  assert(Argc == 2); Scenario = Argv[1];
  memset(&Gu, 0, sizeof(Gu)); memset(&Pci, 0, sizeof(Pci));
  Pci.Pci.Read = ConfigRead; Pci.Pci.Write = ConfigWrite;
  Pci.Mem.Read = MemRead; Pci.Mem.Write = MemWrite;
  Gu.pci = &Pci; Gu.bar_base = GU_PROFILE_BAR_BASE; Gu.bar_size = GU_PROFILE_BAR_SIZE;
  Gu.pcie_cap = 0x70; Gu.started = Gu.quiesced = Gu.saved = TRUE;
  Gu.owned = Gu.armed = TRUE; Gu.flrs = 1; Gu.mmio_writes = 200;
  SetDword(0, GU_BOARD_ID); SetDword(0x2c, GU_BOARD_SUBSYSTEM); SetWord(4, 2);
  LogReady = TRUE;

  if (Is("owned-success")) Success = TRUE;
  else if (Is("unowned-success")) {Gu.owned = Gu.armed = FALSE; Success = TRUE;}
  else if (Is("silent-closed-success")) {Silent = TRUE; LogReady = FALSE; Success = TRUE;}
  else if (Is("silent-open-success")) {Silent = TRUE; Success = TRUE;}
  else if (Is("silent-failure")) {Silent = TRUE; LogReady = FALSE; Registers[7].Value = 7;}
  else if (Is("payload-permissions-success")) {
    Registers[2].Value = 0xfffff0ffU; Registers[3].Value = Registers[4].Value = 0xffffffffU;
    Success = TRUE;
  } else if (Is("reserved-bits-success")) {
    Registers[8].Value = 0xf8000000U; Registers[10].Value = 0x1000ffU;
    Registers[11].Value = 0x10010U; Registers[12].Value = 0x123450U;
    Registers[13].Value = 0xfU; SetWord(0x7aU, 0x1fU); Success = TRUE;
  } else if (Is("not-started")) Gu.started = FALSE;
  else if (Is("not-quiesced")) Gu.quiesced = FALSE;
  else if (Is("not-saved")) Gu.saved = FALSE;
  else if (Is("no-flr")) Gu.flrs = 0;
  else if (Is("two-flrs")) Gu.flrs = 2;
  else if (Is("no-writes")) Gu.mmio_writes = 0;
  else if (Is("fault")) {Gu.fault = TRUE; ExpectFault = TRUE;}
  else if (Is("retained")) Gu.retained = TRUE;
  else if (Is("master-pending")) Gu.master_attributes_pending = TRUE;
  else if (Is("dma-count")) Gu.dma_count = 1;
  else if (Is("allocations")) Gu.allocations = &Allocation;
  else if (Is("master-attribute-error")) Gu.master_attribute_status = EFI_DEVICE_ERROR;
  else if (Is("master-hardware-warning")) Gu.master_hardware_status = EFI_WARN_UNKNOWN_GLYPH;
  else if (Is("null-pci")) Gu.pci = NULL;
  else if (Is("null-config-read")) Pci.Pci.Read = NULL;
  else if (Is("null-mem-read")) Pci.Mem.Read = NULL;
  else if (Is("bar-base")) Gu.bar_base += 0x1000000;
  else if (Is("bar-size")) Gu.bar_size -= 4;
  else if (Is("pcie-missing")) Gu.pcie_cap = 0;
  else if (Is("pcie-unaligned")) Gu.pcie_cap++;
  else if (Is("pcie-overflow")) Gu.pcie_cap = 0xf8;
  else if (Is("log-closed")) {LogReady = FALSE; NoReads = TRUE;}
  else if (Is("wrong-device")) SetDword(0, 0x268410deU);
  else if (Is("wrong-subsystem")) SetDword(0x2c, 0);
  else if (Is("bme")) SetWord(4, 6);
  else if (Is("decode-disabled")) SetWord(4, 0);
  else if (Is("command-inaccessible")) SetWord(4, 0xffffU);
  else if (Is("pending")) SetWord(0x7a, 0x20U);
  else if (Is("pending-inaccessible")) SetWord(0x7a, 0xffffU);
  else if (Is("late-device")) LateIdentity = TRUE;
  else if (Is("late-bme")) LateCommand = TRUE;
  else if (Is("late-pending")) LatePending = TRUE;
  else if (Is("late-chip")) LateChip = TRUE;
  else if (Is("late-lifecycle")) LateLifecycle = TRUE;
  else if (Is("callback-fault")) {CallbackFault = TRUE; ExpectFault = TRUE;}
  else if (sscanf(Scenario, "read-error-%u", &Index) == 1) {
    FailRead = Index; InjectedStatus = EFI_NO_RESPONSE; ExpectFault = TRUE;
  } else if (sscanf(Scenario, "read-warning-%u", &Index) == 1) {
    FailRead = Index; InjectedStatus = EFI_WARN_UNKNOWN_GLYPH; ExpectFault = TRUE;
  } else if (sscanf(Scenario, "log-failure-%u", &Index) == 1) FailLog = Index;
  else if (sscanf(Scenario, "log-disappears-%u", &Index) == 1) CloseLogAtRead = Index;
  else if (sscanf(Scenario, "register-%u-%x", &Index, &Value) == 2) {
    assert(Index < sizeof(Registers)/sizeof(Registers[0])); Registers[Index].Value = Value;
  } else assert(!"unknown scenario");

  if (!Gu.started || !Gu.quiesced || !Gu.saved || Gu.flrs != 1 || !Gu.mmio_writes ||
      Gu.fault || Gu.retained || Gu.master_attributes_pending || Gu.dma_count || Gu.allocations ||
      Gu.master_attribute_status || Gu.master_hardware_status || !Gu.pci || !Pci.Pci.Read ||
      !Pci.Mem.Read || Gu.bar_base != GU_PROFILE_BAR_BASE || Gu.bar_size != GU_PROFILE_BAR_SIZE ||
      Gu.pcie_cap < 0x40U || Gu.pcie_cap > 0xd4U || (Gu.pcie_cap & 3U)) NoReads = TRUE;
  memcpy(BeforeConfig, Config, sizeof(Config)); memcpy(BeforeRegisters, Registers, sizeof(Registers));
  Status = GuStagedPostState(!Silent);
  assert(Success ? Status == EFI_SUCCESS : EFI_ERROR(Status));
  assert(Gu.fault == ExpectFault);
  assert(!Forbidden);
  assert(!memcmp(BeforeConfig, Config, sizeof(Config)));
  assert(!memcmp(BeforeRegisters, Registers, sizeof(Registers)));
  assert(Gu.flrs == (Is("no-flr") ? 0U : Is("two-flrs") ? 2U : 1U));
  assert(Gu.mmio_writes == (Is("no-writes") ? 0U : 200U));
  if (NoReads) assert(!Reads);
  if (Silent) assert(!Logs && !LogQueries);
  if (Success) assert(Reads == 23 && ConfigReads == 8 && MmioReads == 15);
  if (FailRead) {
    assert(Reads == FailRead);
    assert(Status == (EFI_ERROR(InjectedStatus) ? InjectedStatus : EFI_DEVICE_ERROR));
  }
  if (FailLog) assert(Logs == FailLog);
  if (CallbackFault) assert(Reads == 3);
  printf("%s: PASS\n", Scenario);
  return 0;
}
