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
        LogWrite("[Release] Error: DisconnectController failed: %#llx\n", (unsigned long long)status);
        return status;
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
    status = gpu->pci_io->Pci.Read(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &readback);
    if (EFI_ERROR(status)) {
        LogWrite("[Decode] Failed to read back PCI Command register (%#llx)\n", (unsigned long long)status);
        return status;
    }

    LogWrite("[Decode] PCI Command after: %#x (IO=%s, Memory=%s, Master=%s)\n",
             readback, (readback & 1) ? "ENABLED" : "DISABLED",
             (readback & 2) ? "ENABLED" : "DISABLED",
             (readback & 4) ? "ENABLED" : "DISABLED");

    /* Validate readback: Memory Space Enable (bit 1) must be set, Bus Master (bit 2) must be cleared */
    if ((readback & 0x0002) == 0 || (readback & 0x0004) != 0) {
        LogWrite("[Decode] Error: Contradictory PCI Command readback: %#x (expected Memory=1, Master=0)\n", readback);
        return EFI_DEVICE_ERROR;
    }

    return EFI_SUCCESS;
}

EFI_STATUS ReconnectGpuDisplay(DETECTED_GPU *gpu)
{
    if (!gpu || !gpu->handle) {
        return EFI_INVALID_PARAMETER;
    }

    if (!gpu->is_display_adapter) {
        return EFI_SUCCESS;
    }

    LogWrite("[Display] Reconnecting UEFI GOP display driver...\n");

    /* 1. Reconnect GOP driver recursively to the GPU controller handle */
    EFI_STATUS status = gBS->ConnectController(gpu->handle, NULL, NULL, TRUE);
    if (EFI_ERROR(status)) {
        LogWrite("[Display] ConnectController on GPU handle returned %#llx\n", (unsigned long long)status);
    }

    /* 2. Reconnect system console out handle if present */
    if (gST && gST->ConsoleOutHandle) {
        gBS->ConnectController(gST->ConsoleOutHandle, NULL, NULL, TRUE);
    }

    /* 3. Reset and reinitialize UEFI console */
    if (gST && gST->ConOut) {
        gST->ConOut->Reset(gST->ConOut, FALSE);
        gST->ConOut->SetAttribute(gST->ConOut, EFI_LIGHTGRAY | EFI_BLACK);
    }

    /* 4. Ensure PCI Command register has Memory and IO space enabled for Windows */
    if (gpu->pci_io && gpu->pci_io->Pci.Read && gpu->pci_io->Pci.Write) {
        UINT16 cmd = 0;
        gpu->pci_io->Pci.Read(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &cmd);
        if ((cmd & 0x0003) != 0x0003) {
            cmd |= 0x0003;
            gpu->pci_io->Pci.Write(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &cmd);
        }
    }

    LogWrite("[Display] Display GOP and UEFI console reconnected successfully\n");
    return EFI_SUCCESS;
}
