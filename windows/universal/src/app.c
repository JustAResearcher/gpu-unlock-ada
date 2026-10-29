/* SPDX-License-Identifier: GPL-2.0-only */
#include "uefi_base.h"
#include "gpu_types.h"
#include "pci_scan.h"
#include "vbios_loader.h"
#include "vbios_parser.h"
#include "display_release.h"
#include "unlock_engine.h"
#include "logger.h"
#include "asset_loader.h"

EFI_SYSTEM_TABLE        *gST = NULL;
EFI_BOOT_SERVICES       *gBS = NULL;
EFI_RUNTIME_SERVICES    *gRT = NULL;
EFI_HANDLE              gImageHandle = NULL;

static EFI_GUID SimpleFileSystemGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID LoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID DevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;

static DETECTED_GPU g_gpus[MAX_SYSTEM_GPUS];
static UINTN g_gpu_count = 0;

static VOID Print(CONST CHAR16 *fmt)
{
    if (gST && gST->ConOut && gST->ConOut->OutputString) {
        gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)fmt);
    }
}

static VOID PrintColor(UINTN attr, CONST CHAR16 *text)
{
    if (gST && gST->ConOut) {
        gST->ConOut->SetAttribute(gST->ConOut, attr);
        gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)text);
        gST->ConOut->SetAttribute(gST->ConOut, EFI_LIGHTGRAY | EFI_BLACK);
    }
}

static VOID PrintHex32(UINT32 val)
{
    CHAR16 buf[11];
    static const CHAR16 hex[] = L"0123456789ABCDEF";
    buf[0] = L'0'; buf[1] = L'x';
    for (int i = 7; i >= 0; i--) {
        buf[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    buf[10] = L'\0';
    Print(buf);
}

static VOID PrintDec(UINTN val)
{
    CHAR16 buf[16];
    int idx = 14;
    buf[15] = L'\0';
    if (val == 0) {
        Print(L"0");
        return;
    }
    while (val > 0 && idx >= 0) {
        buf[idx--] = L'0' + (val % 10);
        val /= 10;
    }
    Print(&buf[idx + 1]);
}

static EFI_HANDLE FindWindowsBootVolume(VOID)
{
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    EFI_STATUS s = gBS->LocateHandleBuffer(ByProtocol, &SimpleFileSystemGuid, NULL, &count, &handles);
    if (EFI_ERROR(s) || !handles) return NULL;

    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        s = gBS->HandleProtocol(handles[i], &SimpleFileSystemGuid, (VOID **)&fs);
        if (EFI_ERROR(s) || !fs) continue;

        EFI_FILE_HANDLE root = NULL;
        s = fs->OpenVolume(fs, &root);
        if (EFI_ERROR(s) || !root) continue;

        EFI_FILE_HANDLE win_file = NULL;
        s = root->Open(root, &win_file, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi", EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(s) && win_file) {
            win_file->Close(win_file);
            root->Close(root);
            EFI_HANDLE win_vol = handles[i];
            gBS->FreePool(handles);
            return win_vol;
        }
        root->Close(root);
    }

    gBS->FreePool(handles);
    return NULL;
}

static EFI_STATUS BootWindows(VOID)
{
    LoggerClose();

    EFI_HANDLE win_vol = FindWindowsBootVolume();
    if (!win_vol) {
        PrintColor(EFI_LIGHTRED | EFI_BLACK, L"\r\n[!] Error: Windows Boot Manager (bootmgfw.efi) not found on any volume!\r\n");
        return EFI_NOT_FOUND;
    }

    EFI_DEVICE_PATH_PROTOCOL *vol_dp = NULL;
    gBS->HandleProtocol(win_vol, &DevicePathProtocolGuid, (VOID **)&vol_dp);

    /* Construct device path for bootmgfw.efi */
    CONST CHAR16 *win_path = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
    UINTN name_bytes = 34 * sizeof(CHAR16);
    UINTN total_bytes = sizeof(EFI_DEVICE_PATH_PROTOCOL) + name_bytes;

    UINT8 dp_node[sizeof(EFI_DEVICE_PATH_PROTOCOL) + 128];
    EFI_DEVICE_PATH_PROTOCOL *file_dp = (EFI_DEVICE_PATH_PROTOCOL *)dp_node;
    file_dp->Type = MEDIA_DEVICE_PATH;
    file_dp->SubType = MEDIA_FILEPATH_DP;
    file_dp->Length[0] = (UINT8)(total_bytes & 0xFF);
    file_dp->Length[1] = (UINT8)(total_bytes >> 8);

    CHAR16 *dst = (CHAR16 *)(dp_node + sizeof(EFI_DEVICE_PATH_PROTOCOL));
    for (UINTN i = 0; i < 34; i++) dst[i] = win_path[i];

    EFI_HANDLE win_image = NULL;
    EFI_STATUS s = gBS->LoadImage(FALSE, gImageHandle, file_dp, NULL, 0, &win_image);
    if (EFI_ERROR(s)) {
        EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
        s = gBS->HandleProtocol(gImageHandle, &LoadedImageProtocolGuid, (VOID **)&loaded);
        if (!EFI_ERROR(s)) {
            s = gBS->LoadImage(FALSE, win_vol, file_dp, NULL, 0, &win_image);
        }
    }

    if (!EFI_ERROR(s) && win_image) {
        return gBS->StartImage(win_image, NULL, NULL);
    }

    PrintColor(EFI_LIGHTRED | EFI_BLACK, L"\r\n[!] Failed to start Windows Boot Manager.\r\n");
    return s;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gRT = SystemTable->RuntimeServices;
    gImageHandle = ImageHandle;

    /* Initialize persistent file logger on USB: \UNLOCK-LOGS\RUNxxxx.TXT */
    LoggerInit(ImageHandle);

    /* Load V67 firmware payloads from USB into host RAM (Zero-Flash) */
    EFI_STATUS a_status = LoadFirmwareAssets(ImageHandle);
    if (EFI_ERROR(a_status)) {
        LogWrite("[!] LoadFirmwareAssets returned status=%#llx\n", (unsigned long long)a_status);
    }

    if (gST->ConOut) {
        gST->ConOut->ClearScreen(gST->ConOut);
    }

    LogWrite("UNIVERSAL MULTI-GPU WINDOWS UNLOCK ENGINE v2.0 (Zero-Flash, In-RAM)\n");

    PrintColor(EFI_LIGHTCYAN | EFI_BLACK, L"======================================================================\r\n");
    PrintColor(EFI_WHITE | EFI_BLACK,     L"     UNIVERSAL MULTI-GPU WINDOWS UNLOCK ENGINE (AMP/ADA)\r\n");
    PrintColor(EFI_YELLOW | EFI_BLACK,    L"    Safety Guaranteed: 100% Volatile In-RAM Unlock (Zero SPI Flash Writes)\r\n");
    PrintColor(EFI_LIGHTCYAN | EFI_BLACK, L"======================================================================\r\n\r\n");

    /* 1. PCIe Topology Multi-GPU Scan */
    g_gpu_count = ScanSystemGpus(g_gpus, MAX_SYSTEM_GPUS);
    LogWrite("ScanSystemGpus found %u supported GPU(s)\n", (unsigned)g_gpu_count);

    if (g_gpu_count == 0) {
        PrintColor(EFI_LIGHTRED | EFI_BLACK, L"[!] No supported NVIDIA RTX 30 or 40-Series GPUs detected.\r\n");
        LogWrite("[!] No supported NVIDIA GPUs detected.\n");
        Print(L"    Booting stock Windows in 3 seconds...\r\n");
        gBS->Stall(3000000);
        return BootWindows();
    }

    Print(L"Detected Supported GPUs: ");
    PrintDec(g_gpu_count);
    Print(L"\r\n----------------------------------------------------------------------\r\n");

    /* 2. Enumerate GPUs and Load Strict ID VBIOS ROMs from USB */
    UINTN ready_to_unlock = 0;
    for (UINTN i = 0; i < g_gpu_count; i++) {
        DETECTED_GPU *gpu = &g_gpus[i];

        LogWrite("[GPU %u] %ls (%ls) Bus=%u:%u.%u Vendor=%#x Dev=%#x SubVen=%#x SubDev=%#x BAR0=%#llx Display=%u\n",
                 (unsigned)i, gpu->model_name, gpu->chip_name,
                 (unsigned)gpu->bus, (unsigned)gpu->device, (unsigned)gpu->function,
                 gpu->vendor_id, gpu->device_id, gpu->subsys_vendor_id, gpu->subsys_device_id,
                 (unsigned long long)gpu->bar0_base, gpu->is_display_adapter);

        Print(L"[GPU "); PrintDec(i); Print(L"] ");
        PrintColor(EFI_WHITE | EFI_BLACK, gpu->model_name);
        Print(L" ("); Print(gpu->chip_name); Print(L") on Bus ");
        PrintDec(gpu->bus); Print(L":"); PrintDec(gpu->device); Print(L"."); PrintDec(gpu->function);
        Print(L"\r\n        ID: "); Print(gpu->rom_filename + 10);
        Print(L" | BAR0: "); PrintHex32((UINT32)gpu->bar0_base);

        if (gpu->is_display_adapter) {
            PrintColor(EFI_LIGHTMAGENTA | EFI_BLACK, L" [Display Adapter (GOP)]\r\n");
        } else {
            PrintColor(EFI_LIGHTGREEN | EFI_BLACK, L" [Headless Compute Accelerator]\r\n");
        }

        /* Strict ID VBIOS Loading */
        EFI_STATUS v_status = LoadGpuVbiosStrictId(gpu, g_gpus, g_gpu_count);
        if (!EFI_ERROR(v_status) && gpu->vbios_loaded) {
            Print(L"        VBIOS: ");
            Print(gpu->rom_filename);
            Print(L" ("); PrintDec(gpu->vbios_size / 1024); Print(L" KB) -> ");
            PrintColor(EFI_LIGHTGREEN | EFI_BLACK, L"[LOADED IN RAM]\r\n");
            ready_to_unlock++;
        } else {
            Print(L"        VBIOS: ");
            Print(gpu->rom_filename);
            Print(L" -> ");
            PrintColor(EFI_LIGHTRED | EFI_BLACK, L"[NOT FOUND ON USB]\r\n");
            Print(L"               (Run dump_vbios.bat in Windows to dump by ID)\r\n");
        }
    }

    Print(L"----------------------------------------------------------------------\r\n\r\n");

    /* 3. Interactive Menu */
    PrintColor(EFI_YELLOW | EFI_BLACK, L"Boot Options:\r\n");
    Print(L"  [U] Unlock All Ready GPUs & Boot Windows (Automatic in 30s)\r\n");
    Print(L"  [S] Boot Stock Windows (Bypass Unlock)\r\n\r\n");
    Print(L"Choice [U/S]: ");

    EFI_INPUT_KEY key;
    BOOLEAN unlock = TRUE;
    UINTN seconds = 30;

    while (seconds > 0) {
        EFI_STATUS ks = gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
        if (!EFI_ERROR(ks)) {
            if (key.UnicodeChar == L's' || key.UnicodeChar == L'S') {
                unlock = FALSE;
                PrintColor(EFI_LIGHTCYAN | EFI_BLACK, L"S (Stock Windows selected)\r\n");
                LogWrite("Operator choice: S (Stock)\n");
                break;
            }
            if (key.UnicodeChar == L'u' || key.UnicodeChar == L'U' || key.UnicodeChar == 13) {
                unlock = TRUE;
                PrintColor(EFI_LIGHTGREEN | EFI_BLACK, L"U (Unlock selected)\r\n");
                LogWrite("Operator choice: U (Unlock)\n");
                break;
            }
        }
        gBS->Stall(1000000);
        seconds--;
    }

    if (unlock && ready_to_unlock > 0) {
        PrintColor(EFI_LIGHTCYAN | EFI_BLACK, L"\r\nApplying In-RAM Unlocks (Zero-Flash)...\r\n");
        LogWrite("Executing In-RAM unlock sequence for %u ready GPU(s)\n", (unsigned)ready_to_unlock);

        for (UINTN i = 0; i < g_gpu_count; i++) {
            DETECTED_GPU *gpu = &g_gpus[i];
            if (!gpu->vbios_loaded) continue;

            Print(L"  [+] Unlocking GPU "); PrintDec(i); Print(L" ("); Print(gpu->model_name); Print(L")... ");

            if (gpu->is_display_adapter) {
                LogWrite("Releasing display on GPU %u...\n", (unsigned)i);
                ReleaseGpuDisplay(gpu);
            }

            EFI_STATUS u_stat = ExecuteGpuRamUnlock(gpu);
            if (!EFI_ERROR(u_stat) && gpu->unlocked_success) {
                PrintColor(EFI_LIGHTGREEN | EFI_BLACK, L"[SUCCESS: SS0/SS1 LATCHED]\r\n");
                LogWrite("GPU %u unlocked successfully! SS0=0x88888888 SS1=0x8\n", (unsigned)i);
            } else {
                PrintColor(EFI_LIGHTRED | EFI_BLACK, L"[FAILED]\r\n");
                LogWrite("GPU %u unlock failed: status=%#llx\n", (unsigned)i, (unsigned long long)u_stat);
            }
        }
    } else {
        Print(L"\r\nSkipping unlock; booting stock configuration.\r\n");
        LogWrite("Unlock skipped; booting stock.\n");
    }

    PrintColor(EFI_WHITE | EFI_BLACK, L"\r\nLaunching Windows Boot Manager...\r\n");
    gBS->Stall(1000000);

    return BootWindows();
}
