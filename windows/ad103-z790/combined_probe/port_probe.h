/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Read-only subset of windows/ad104-msi/port.h, for a diagnostic probe only.
 *
 * Deliberately missing compared to port.h: GuWrite32, GuFlr, GuEnableMaster,
 * GuClearMaster, GuDmaAllocate/GuDmaFree, GuConfigWrite, GuSavePci/GuRestorePci,
 * the selector-latch functions, and everything msi_release.c/boot_io.c do for
 * console/display/USB ownership and Windows staging. This probe cannot write
 * to the GPU, cannot touch PCI config space, cannot perform FLR, and does not
 * load, stage or start anything -- it only reads and prints.
 */
#pragma once
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <stddef.h>

/* RTX 4070 Ti SUPER, 10de:2705, subsystem 1462:513e (from gpu-identity.csv,
 * same encoding convention as ad104-msi's GU_BOARD_ID/GU_BOARD_SUBSYSTEM:
 * (device<<16|vendor) and (subsystem_device<<16|subsystem_vendor)). */
#define GU_BOARD_ID 0x270510deU
#define GU_BOARD_SUBSYSTEM 0x513e1462U
/* Architectural BAR0 window size for this GPU family -- same value ad104-msi
 * uses, not board-specific. Only bounds GuRead32; this probe never needs the
 * actual physical BAR0 base address, because GuRead32 addresses relative to
 * the BAR via EFI_PCI_IO_PROTOCOL, not by dereferencing a raw pointer. */
#define GU_BAR0_WINDOW 0x1000000ULL

typedef struct {
  EFI_BOOT_SERVICES *bs;
  EFI_PCI_IO_PROTOCOL *pci;
  EFI_HANDLE image, handle;
  UINT64 bar_size;
  UINT16 pm_cap, pcie_cap, msi_cap, msix_cap;
  UINT8 pcie_version;
  BOOLEAN fault;
} GU_PROBE;
extern GU_PROBE Gu;

VOID GuInit(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci);
/* Read-only: EFI_PCI_IO_PROTOCOL.Pci.Read, not .Write. */
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, VOID *Value);
/* Read-only: EFI_PCI_IO_PROTOCOL.Mem.Read against BAR index 0, not .Write. */
UINT32 GuRead32(UINT32 Offset);
