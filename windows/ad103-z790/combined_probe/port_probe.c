/* SPDX-License-Identifier: GPL-2.0-only */
#include "port_probe.h"
VOID *memset(VOID *, int, size_t);
GU_PROBE Gu;

VOID GuInit(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci) {
  memset(&Gu, 0, sizeof(Gu));
  Gu.bs = Bs;
  Gu.pci = Pci;
  Gu.bar_size = GU_BAR0_WINDOW;
}

/* Config space is CPU-owned, up to 256 bytes. Read-only: never calls Pci.Write. */
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, VOID *Value) {
  if (Gu.fault) return EFI_DEVICE_ERROR;
  if (!Gu.pci || Width > EfiPciIoWidthUint32 || Offset > 256 - ((UINT32)1 << Width)) return EFI_INVALID_PARAMETER;
  EFI_STATUS Status = Gu.pci->Pci.Read(Gu.pci, Width, Offset, 1, Value);
  if (EFI_ERROR(Status)) Gu.fault = TRUE;
  return Status;
}

/* BAR0, relative addressing via EFI_PCI_IO_PROTOCOL -- never dereferences a raw
 * pointer, so this probe never needs to know the physical BAR0 base address.
 * Read-only: never calls Mem.Write. */
UINT32 GuRead32(UINT32 Offset) {
  UINT32 Value = 0xffffffffU;
  if (Gu.fault) return Value;
  if (!Gu.pci || (Offset & 3) || (UINT64)Offset + 4 > Gu.bar_size ||
      EFI_ERROR(Gu.pci->Mem.Read(Gu.pci, EfiPciIoWidthUint32, 0, Offset, 1, &Value))) {
    Gu.fault = TRUE;
    return 0xffffffffU;
  }
  return Value;
}
