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

static BOOLEAN CheckFileExistsOnBootVolume(CONST CHAR16 *path)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_STATUS s = gBS->HandleProtocol(gImageHandle, &loaded_guid, (VOID **)&loaded);
    if (EFI_ERROR(s) || !loaded || !loaded->DeviceHandle) return FALSE;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    s = gBS->HandleProtocol(loaded->DeviceHandle, &fs_guid, (VOID **)&fs);
    if (EFI_ERROR(s) || !fs) return FALSE;

    EFI_FILE_HANDLE root = NULL;
    s = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(s) || !root) return FALSE;

    EFI_FILE_HANDLE f = NULL;
    s = root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(s) && f) {
        f->Close(f);
        root->Close(root);
        return TRUE;
    }
    root->Close(root);
    return FALSE;
}

EFI_STATUS ReconnectGpuDisplay(DETECTED_GPU *gpu)
{
    if (!gpu || !gpu->handle) {
        return EFI_INVALID_PARAMETER;
    }

    if (!gpu->is_display_adapter) {
        return EFI_SUCCESS;
    }

    /* 1. Ensure PCI Command has IO (0x1) and Memory (0x2) enabled for Windows handoff */
    if (gpu->pci_io && gpu->pci_io->Pci.Read && gpu->pci_io->Pci.Write) {
        UINT16 cmd = 0;
        gpu->pci_io->Pci.Read(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &cmd);
        cmd = (cmd & ~0x0004) | 0x0003; /* Enable IO + Memory decode, disable Bus Master */
        gpu->pci_io->Pci.Write(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &cmd);
        LogWrite("[Display] PCI Command primed for Windows handoff: %#06x\n", cmd);
    }

    /* 2. Check if user explicitly requested firmware ConnectController via tx-force-reconnect.txt */
    BOOLEAN force_reconnect = CheckFileExistsOnBootVolume(L"\\tx-force-reconnect.txt") ||
                              CheckFileExistsOnBootVolume(L"tx-force-reconnect.txt");

    if (!force_reconnect) {
        LogWrite("[Display] Skipping firmware ConnectController by default.\n"
                 "          Rationale: Option ROM GOP driver post-FLR causes UEFI deadlocks on modern platforms.\n"
                 "          Handoff design: Windows Boot Manager (bootmgfw.efi) and nvlddmkm.sys initialize display natively.\n");
        return EFI_SUCCESS;
    }

    /* 3. Forced reconnect branch (only if explicitly enabled by user via tx-force-reconnect.txt) */
    BOOLEAN force_recursive = CheckFileExistsOnBootVolume(L"\\tx-reconnect-recursive.txt") ||
                              CheckFileExistsOnBootVolume(L"tx-reconnect-recursive.txt");

    LogWrite("[Display] Attempting FORCED GOP reconnect (recursive=%s)...\n",
             force_recursive ? "TRUE" : "FALSE");

    if (gpu->pci_io && gpu->pci_io->Pci.Read && gpu->pci_io->Pci.Write) {
        UINT16 cmd = 0;
        gpu->pci_io->Pci.Read(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &cmd);
        cmd |= 0x0007; /* Enable IO, Memory, and Bus Master for GOP */
        gpu->pci_io->Pci.Write(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &cmd);
    }

    gBS->Stall(200000);
    EFI_STATUS status = gBS->ConnectController(gpu->handle, NULL, NULL, force_recursive);
    LogWrite("[Display] ConnectController on GPU handle returned status=%#llx\n", (unsigned long long)status);

    if (force_recursive) {
        if (gST && gST->ConsoleOutHandle) {
            LogWrite("[Display] Reconnecting ConsoleOutHandle...\n");
            gBS->ConnectController(gST->ConsoleOutHandle, NULL, NULL, TRUE);
        }
        if (gST && gST->ConOut) {
            LogWrite("[Display] Resetting console...\n");
            gST->ConOut->Reset(gST->ConOut, FALSE);
            gST->ConOut->SetAttribute(gST->ConOut, EFI_LIGHTGRAY | EFI_BLACK);
        }
    }

    return EFI_SUCCESS;
}
