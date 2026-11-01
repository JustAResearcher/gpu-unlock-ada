/* Modified 2026-10-19/20 for Windows GPU UNLOCK; changes documented in
 * CHANGES.md. */
#include "checks.h"
#include "logger.h"
#include <IndustryStandard/Pci.h>

#define GU_CONFIG_BYTES 256U
#define GU_CAP_START 0x40U
#define GU_CAP_LAST 0xfcU
#define GU_CAP_LIMIT ((GU_CONFIG_BYTES - GU_CAP_START) / 4U)
#define GU_MSIX_ID 0x11U

static UINT8 ConfigByte(const UINT32 Config[64], UINTN Offset) {
  return (UINT8)(Config[Offset / 4U] >> ((Offset & 3U) * 8U));
}

static UINT16 ConfigWord(const UINT32 Config[64], UINTN Offset) {
  return (UINT16)(ConfigByte(Config, Offset) |
                  ((UINT16)ConfigByte(Config, Offset + 1U) << 8U));
}

BOOLEAN GuCheckConfig(const UINT32 Config[64], UINTN Segment, UINTN Bus,
                      UINTN Device, UINTN Function, GU_CAPS *Out) {
  GU_CAPS Caps = {0, 0, 0, 0, 0};
  UINT8 Occupied[GU_CONFIG_BYTES - GU_CAP_START] = {0};
  UINT64 Seen = 0;
  UINTN Pointer, Steps = 0;
  UINT16 Command;

  if (Out == NULL || Config == NULL) {
    LogWrite("[GuCheckConfig] Bus %u:%u.%u: Error - NULL parameter passed "
             "(Out=%p Config=%p)\n",
             (unsigned)Bus, (unsigned)Device, (unsigned)Function, Out, Config);
    return FALSE;
  }
  *Out = Caps;
  (void)Segment;

  if (ConfigByte(Config, 0x0b) != PCI_CLASS_DISPLAY) {
    LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Class byte %#x != "
             "PCI_CLASS_DISPLAY (0x03)\n",
             (unsigned)Bus, (unsigned)Device, (unsigned)Function,
             ConfigByte(Config, 0x0b));
    return FALSE;
  }

  if ((ConfigByte(Config, PCI_HEADER_TYPE_OFFSET) & 0x7fU) !=
      HEADER_TYPE_DEVICE) {
    LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Header type %#x != "
             "HEADER_TYPE_DEVICE (0x00)\n",
             (unsigned)Bus, (unsigned)Device, (unsigned)Function,
             ConfigByte(Config, PCI_HEADER_TYPE_OFFSET) & 0x7fU);
    return FALSE;
  }

  Command = ConfigWord(Config, PCI_COMMAND_OFFSET);
  UINT16 Status = ConfigWord(Config, PCI_PRIMARY_STATUS_OFFSET);
  UINT32 RomBase = Config[PCI_EXPANSION_ROM_BASE / 4];

  if ((Command & EFI_PCI_COMMAND_MEMORY_SPACE) == 0) {
    LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Memory Space disabled "
             "in Command (%#06x, bit 1 is 0)\n",
             (unsigned)Bus, (unsigned)Device, (unsigned)Function, Command);
    return FALSE;
  }

  if ((Command & EFI_PCI_COMMAND_BUS_MASTER) != 0) {
    LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Bus Master enabled in "
             "Command (%#06x, bit 2 is 1)\n",
             (unsigned)Bus, (unsigned)Device, (unsigned)Function, Command);
    return FALSE;
  }

  if ((RomBase & 1U) != 0) {
    LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Expansion ROM decode "
             "active (%#x, bit 0 is 1)\n",
             (unsigned)Bus, (unsigned)Device, (unsigned)Function, RomBase);
    return FALSE;
  }

  if ((Status & EFI_PCI_STATUS_CAPABILITY) == 0) {
    LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - PCI Status (%#06x) "
             "lacks Capabilities List (bit 4 is 0)\n",
             (unsigned)Bus, (unsigned)Device, (unsigned)Function, Status);
    return FALSE;
  }

  Pointer = ConfigByte(Config, PCI_CAPABILITY_POINTER_OFFSET);
  while (Pointer != 0) {
    UINTN Length, I;
    UINT16 Control;
    UINT8 Id, Next;
    UINT64 Bit;
    if (++Steps > GU_CAP_LIMIT) {
      LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Cap list step count "
               "(%zu) exceeded limit (%u)\n",
               (unsigned)Bus, (unsigned)Device, (unsigned)Function, Steps,
               GU_CAP_LIMIT);
      return FALSE;
    }
    if (Pointer < GU_CAP_START || Pointer > GU_CAP_LAST ||
        (Pointer & 3U) != 0) {
      LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Invalid cap pointer "
               "%#zx (expected 0x40-0xfc, 4-byte aligned)\n",
               (unsigned)Bus, (unsigned)Device, (unsigned)Function, Pointer);
      return FALSE;
    }
    Bit = (UINT64)1U << (Pointer / 4U);
    if ((Seen & Bit) != 0) {
      LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Cap loop detected at "
               "offset %#zx\n",
               (unsigned)Bus, (unsigned)Device, (unsigned)Function, Pointer);
      return FALSE;
    }
    Seen |= Bit;

    Id = ConfigByte(Config, Pointer);
    Next = ConfigByte(Config, Pointer + 1U);
    Control = ConfigWord(Config, Pointer + 2U);

    switch (Id) {
    case EFI_PCI_CAPABILITY_ID_PMI:
      if (Caps.pm != 0 || (Control & 7U) == 0 || (Control & 7U) > 3) {
        LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Invalid PMI cap at "
                 "%#zx (already=%u, Control=%#x)\n",
                 (unsigned)Bus, (unsigned)Device, (unsigned)Function, Pointer,
                 Caps.pm, Control);
        return FALSE;
      }
      Length = 8;
      break;
    case EFI_PCI_CAPABILITY_ID_MSI:
      if (Caps.msi != 0 || (Control & 1U) != 0) {
        LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Invalid MSI cap at "
                 "%#zx (already=%u, Control=%#x)\n",
                 (unsigned)Bus, (unsigned)Device, (unsigned)Function, Pointer,
                 Caps.msi, Control);
        return FALSE;
      }
      /* Mask-capable layouts include two reserved bytes after Message Data. */
      Length = (Control & 0x80U) != 0 ? 14U : 10U;
      if ((Control & 0x100U) != 0)
        Length += 10U;
      break;
    case PCI_EXPRESS_CAPABILITY_ID:
      if (Caps.pcie != 0 || (Control & 0x0fU) < 1 || (Control & 0x0fU) > 2 ||
          ((Control >> 4U) & 0x0fU) > 1) {
        LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Invalid PCIe cap at "
                 "%#zx (already=%u, Control=%#x, ver=%u, type=%u)\n",
                 (unsigned)Bus, (unsigned)Device, (unsigned)Function, Pointer,
                 Caps.pcie, Control, (Control & 0x0fU),
                 ((Control >> 4U) & 0x0fU));
        return FALSE;
      }
      /* Endpoint v1 reaches Link Status; reserve the entire v2 layout. */
      Length = (Control & 0x0fU) == 1 ? 0x14U : 0x3cU;
      break;
    case GU_MSIX_ID:
      if (Caps.msix != 0 || (Control & 0x8000U) != 0) {
        LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Invalid MSI-X cap "
                 "at %#zx (already=%u, Control=%#x)\n",
                 (unsigned)Bus, (unsigned)Device, (unsigned)Function, Pointer,
                 Caps.msix, Control);
        return FALSE;
      }
      Length = 12;
      break;
    case EFI_PCI_CAPABILITY_ID_VENDOR:
      Length = Control & 0xffU;
      if (Length < 3) {
        LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Vendor cap length < "
                 "3 (%zu) at %#zx\n",
                 (unsigned)Bus, (unsigned)Device, (unsigned)Function, Length,
                 Pointer);
        return FALSE;
      }
      break;
    default:
      /* A format we do not understand cannot be checked for overlap/length. */
      LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Unknown/unhandled PCI "
               "capability ID %#04x at %#zx (Next=%#x Control=%#06x)\n",
               (unsigned)Bus, (unsigned)Device, (unsigned)Function, Id, Pointer,
               Next, Control);
      return FALSE;
    }

    if (Length > GU_CONFIG_BYTES - Pointer) {
      LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Cap ID %#x at %#zx "
               "length %zu exceeds config space (%u)\n",
               (unsigned)Bus, (unsigned)Device, (unsigned)Function, Id, Pointer,
               Length, GU_CONFIG_BYTES);
      return FALSE;
    }
    for (I = Pointer; I < Pointer + Length; ++I) {
      if (Occupied[I - GU_CAP_START] != 0) {
        LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Cap ID %#x at %#zx "
                 "overlaps occupied byte %#zx\n",
                 (unsigned)Bus, (unsigned)Device, (unsigned)Function, Id,
                 Pointer, I);
        return FALSE;
      }
      Occupied[I - GU_CAP_START] = 1;
    }

    switch (Id) {
    case EFI_PCI_CAPABILITY_ID_PMI:
      if ((ConfigWord(Config, Pointer + 4U) & 3U) != PCI_POWER_STATE_D0) {
        LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - PMI power state != "
                 "D0 (%#x) at %#zx\n",
                 (unsigned)Bus, (unsigned)Device, (unsigned)Function,
                 ConfigWord(Config, Pointer + 4U) & 3U, Pointer);
        return FALSE;
      }
      Caps.pm = (UINT16)Pointer;
      break;
    case EFI_PCI_CAPABILITY_ID_MSI:
      Caps.msi = (UINT16)Pointer;
      break;
    case PCI_EXPRESS_CAPABILITY_ID:
      if ((Config[(Pointer + 4U) / 4U] & (1U << 28U)) == 0) {
        LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - PCIe lacks FLR "
                 "capability (DevCaps=%#010x, bit 28 is 0) at %#zx\n",
                 (unsigned)Bus, (unsigned)Device, (unsigned)Function,
                 Config[(Pointer + 4U) / 4U], Pointer);
        return FALSE;
      }
      if ((ConfigWord(Config, Pointer + 8U) & 0x8000U) != 0) {
        LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - PCIe DevCtl has FLR "
                 "bit 0x8000 active at %#zx\n",
                 (unsigned)Bus, (unsigned)Device, (unsigned)Function, Pointer);
        return FALSE;
      }
      Caps.pcie = (UINT16)Pointer;
      Caps.pcie_version = (UINT8)(Control & 0x0fU);
      break;
    case GU_MSIX_ID:
      Caps.msix = (UINT16)Pointer;
      break;
    default:
      break;
    }
    Pointer = Next;
  }

  if (Caps.pm == 0 || Caps.pcie == 0) {
    LogWrite("[GuCheckConfig] Bus %u:%u.%u: Rejected - Missing mandatory "
             "capabilities (PM=%#x PCIe=%#x)\n",
             (unsigned)Bus, (unsigned)Device, (unsigned)Function, Caps.pm,
             Caps.pcie);
    return FALSE;
  }

  LogWrite("[GuCheckConfig] Bus %u:%u.%u: Verified OK (PM=%#x PCIe=%#x v%u "
           "MSI=%#x MSIX=%#x)\n",
           (unsigned)Bus, (unsigned)Device, (unsigned)Function, Caps.pm,
           Caps.pcie, Caps.pcie_version, Caps.msi, Caps.msix);
  *Out = Caps;
  return TRUE;
}

static UINT16 Read16(const UINT8 *Bytes) {
  return (UINT16)(Bytes[0] | ((UINT16)Bytes[1] << 8U));
}

static UINT32 Read32(const UINT8 *Bytes) {
  return (UINT32)Read16(Bytes) | ((UINT32)Read16(Bytes + 2) << 16U);
}

static UINT16 NodeLength(const UINT8 *Bytes, UINTN Remaining) {
  UINT16 Length;
  if (Remaining < sizeof(EFI_DEVICE_PATH_PROTOCOL))
    return 0;
  Length = Read16(Bytes + 2);
  if (Length < sizeof(EFI_DEVICE_PATH_PROTOCOL) || Length > Remaining)
    return 0;
  return Length;
}

static BOOLEAN TakePci(const UINT8 **Bytes, UINTN *Remaining, UINT8 Device,
                       UINT8 Function) {
  const UINT8 *Node = *Bytes;
  if (NodeLength(Node, *Remaining) != sizeof(PCI_DEVICE_PATH) ||
      Node[0] != HARDWARE_DEVICE_PATH || Node[1] != HW_PCI_DP ||
      Node[4] != Function || Node[5] != Device)
    return FALSE;
  *Bytes += sizeof(PCI_DEVICE_PATH);
  *Remaining -= sizeof(PCI_DEVICE_PATH);
  return TRUE;
}

int GuDisplayPath(const EFI_DEVICE_PATH_PROTOCOL *Path, UINTN MaxBytes) {
  const UINT8 *Bytes = (const UINT8 *)Path;
  UINT32 Hid;
  int Route;
  if (Path == NULL ||
      NodeLength(Bytes, MaxBytes) != sizeof(ACPI_HID_DEVICE_PATH) ||
      Bytes[0] != ACPI_DEVICE_PATH || Bytes[1] != ACPI_DP)
    return 0;
  Hid = Read32(Bytes + 4);
  if (Hid != EISA_PNP_ID(0x0a03) || Read32(Bytes + 8) != 0)
    return 0;
  Bytes += sizeof(ACPI_HID_DEVICE_PATH);
  MaxBytes -= sizeof(ACPI_HID_DEVICE_PATH);

  if (TakePci(&Bytes, &MaxBytes, 8, 1) && TakePci(&Bytes, &MaxBytes, 0, 0)) {
    Route = 1;
  } else if (TakePci(&Bytes, &MaxBytes, 1, 1) &&
             TakePci(&Bytes, &MaxBytes, 0, 0)) {
    Route = 2;
  } else {
    return 0;
  }
  if (Route == 1) {
    if (NodeLength(Bytes, MaxBytes) != sizeof(ACPI_ADR_DEVICE_PATH) ||
        Bytes[0] != ACPI_DEVICE_PATH || Bytes[1] != ACPI_ADR_DP ||
        Read32(Bytes + 4) != 0x80010300U)
      return 0;
    Bytes += sizeof(ACPI_ADR_DEVICE_PATH);
    MaxBytes -= sizeof(ACPI_ADR_DEVICE_PATH);
    if (NodeLength(Bytes, MaxBytes) != 4 || Bytes[0] != END_DEVICE_PATH_TYPE ||
        Bytes[1] != END_ENTIRE_DEVICE_PATH_SUBTYPE)
      return 0;
    return Route;
  }
  for (;;) {
    UINT16 Length = NodeLength(Bytes, MaxBytes);
    if (Length == 0)
      return 0;
    if (Bytes[0] == END_DEVICE_PATH_TYPE) {
      if (Bytes[1] != END_ENTIRE_DEVICE_PATH_SUBTYPE ||
          Length != sizeof(EFI_DEVICE_PATH_PROTOCOL))
        return 0;
      return Route;
    }
    if (Bytes[0] != ACPI_DEVICE_PATH || Bytes[1] != ACPI_ADR_DP ||
        Length < sizeof(ACPI_ADR_DEVICE_PATH) || (Length & 3U) != 0)
      return 0;
    Bytes += Length;
    MaxBytes -= Length;
  }
}
