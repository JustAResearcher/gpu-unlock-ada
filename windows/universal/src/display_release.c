/* SPDX-License-Identifier: GPL-2.0-only */
#include "display_release.h"
#include "logger.h"
#include "uefi_base.h"

EFI_STATUS ReleaseGpuDisplay(DETECTED_GPU *gpu)
{
    if (!gpu || !gpu->handle || !gpu->pci_io) {
        return EFI_INVALID_PARAMETER;
    }

    /* If this GPU is not holding GOP display console, no need to touch display drivers! */
    if (!gpu->is_display_adapter) {
        return EFI_SUCCESS;
    }

    LogWrite("[Release] Disconnecting UEFI GOP controller from handle...\n");

    /* Standard UEFI DisconnectController - completely board-agnostic */
    EFI_STATUS status = gBS->DisconnectController(gpu->handle, NULL, NULL);
    if (EFI_ERROR(status) && status != EFI_NOT_FOUND) {
        LogWrite("[Release] DisconnectController status=%#llx (ignoring)\n", (unsigned long long)status);
    }

    /* Restore PCI Command register so MMIO / DMA continue functioning */
    return RestoreGpuPciDecode(gpu);
}

EFI_STATUS RestoreGpuPciDecode(DETECTED_GPU *gpu)
{
    if (!gpu || !gpu->pci_io || !gpu->pci_io->Pci.Read || !gpu->pci_io->Pci.Write) {
        return EFI_INVALID_PARAMETER;
    }

    UINT16 cmd = 0;
    /* Offset 4 = PCI Command Register. Count = 1 UINT16. */
    EFI_STATUS status = gpu->pci_io->Pci.Read(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &cmd);
    if (EFI_ERROR(status)) {
        LogWrite("[Decode] Failed to read PCI Command register (%#llx)\n", (unsigned long long)status);
        return status;
    }

    LogWrite("[Decode] PCI Command before: %#x\n", cmd);

    /* Bit 0: IO Space Enable (0x1), Bit 1: Memory Space Enable (0x2).
     * Bit 2 (Bus Master Enable) MUST BE 0 before Falcon transaction claim.
     * The transaction enables Bus Master itself when DMA buffers are mapped.
     */
    cmd = (cmd & ~0x0004) | 0x0003;
    status = gpu->pci_io->Pci.Write(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &cmd);
    if (EFI_ERROR(status)) {
        LogWrite("[Decode] Failed to write PCI Command register (%#llx)\n", (unsigned long long)status);
        return status;
    }

    UINT16 readback = 0;
    gpu->pci_io->Pci.Read(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &readback);
    LogWrite("[Decode] PCI Command after: %#x (IO=%s, Memory=%s, Master=%s)\n",
             readback, (readback & 1) ? "ENABLED" : "DISABLED",
             (readback & 2) ? "ENABLED" : "DISABLED",
             (readback & 4) ? "ENABLED" : "DISABLED");

    return EFI_SUCCESS;
}
