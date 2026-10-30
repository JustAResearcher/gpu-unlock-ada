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
#include "port.h"
#include "runtime.h"

EFI_SYSTEM_TABLE        *gST = NULL;
EFI_BOOT_SERVICES       *gBS = NULL;
EFI_RUNTIME_SERVICES    *gRT = NULL;
EFI_HANDLE              gImageHandle = NULL;

static EFI_GUID SimpleFileSystemGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
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

    CONST CHAR16 *win_path = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
    CONST UINTN chars_count = 33;
    CONST UINTN name_bytes = chars_count * sizeof(CHAR16);
    CONST UINTN file_node_len = sizeof(EFI_DEVICE_PATH_PROTOCOL) + name_bytes;

    /* 1. Retrieve volume device path from the Windows volume handle */
    EFI_DEVICE_PATH_PROTOCOL *vol_dp = NULL;
    EFI_STATUS s = gBS->HandleProtocol(win_vol, &DevicePathProtocolGuid, (VOID **)&vol_dp);

    UINTN vol_dp_len = 0;
    if (!EFI_ERROR(s) && vol_dp) {
        EFI_DEVICE_PATH_PROTOCOL *node = vol_dp;
        while (node && node->Type != END_DEVICE_PATH_TYPE) {
            UINT16 nlen = (UINT16)node->Length[0] | ((UINT16)node->Length[1] << 8);
            if (nlen < sizeof(EFI_DEVICE_PATH_PROTOCOL)) break;
            vol_dp_len += nlen;
            node = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)node + nlen);
        }
    }

    /* 2. Construct complete composite device path: [vol_dp] + [file_node] + [end_node] */
    UINTN total_len = vol_dp_len + file_node_len + 4;
    EFI_DEVICE_PATH_PROTOCOL *composite_dp = NULL;
    s = gBS->AllocatePool(EfiBootServicesData, total_len, (VOID **)&composite_dp);
    if (EFI_ERROR(s) || !composite_dp) {
        PrintColor(EFI_LIGHTRED | EFI_BLACK, L"\r\n[!] Error: Failed to allocate device path buffer.\r\n");
        return s;
    }

    memset(composite_dp, 0, total_len);

    if (vol_dp_len > 0 && vol_dp) {
        memcpy(composite_dp, vol_dp, vol_dp_len);
    }

    EFI_DEVICE_PATH_PROTOCOL *file_node = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)composite_dp + vol_dp_len);
    file_node->Type = MEDIA_DEVICE_PATH;
    file_node->SubType = MEDIA_FILEPATH_DP;
    file_node->Length[0] = (UINT8)(file_node_len & 0xFF);
    file_node->Length[1] = (UINT8)(file_node_len >> 8);

    CHAR16 *dst = (CHAR16 *)((UINT8 *)file_node + sizeof(EFI_DEVICE_PATH_PROTOCOL));
    for (UINTN i = 0; i < chars_count; i++) dst[i] = win_path[i];

    EFI_DEVICE_PATH_PROTOCOL *end_node = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)file_node + file_node_len);
    end_node->Type = END_DEVICE_PATH_TYPE;
    end_node->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end_node->Length[0] = 4;
    end_node->Length[1] = 0;

    /* 3. Load Windows Boot Manager image using gImageHandle as ParentImageHandle */
    EFI_HANDLE win_image = NULL;
    s = gBS->LoadImage(FALSE, gImageHandle, composite_dp, NULL, 0, &win_image);

    gBS->FreePool(composite_dp);

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

    if (gST->ConOut) {
        gST->ConOut->ClearScreen(gST->ConOut);
    }

    /* Initialize persistent file logger on USB: \UNLOCK-LOGS\RUNxxxx.TXT */
    LoggerInit(ImageHandle);

    /* Load V67 firmware payloads from USB into host RAM (Zero-Flash) */
    EFI_STATUS a_status = LoadFirmwareAssets(ImageHandle);
    if (EFI_ERROR(a_status)) {
        LogWrite("[!] LoadFirmwareAssets returned status=%#llx\n", (unsigned long long)a_status);
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

        LogWrite("[GPU %u] %ls (%ls) Bus=%u:%u.%u Vendor=%#x Dev=%#x SubVen=%#x SubDev=%#x BAR0=%#llx Display=%u GOP=%u Console=%u\n",
                 (unsigned)i, gpu->model_name, gpu->chip_name,
                 (unsigned)gpu->bus, (unsigned)gpu->device, (unsigned)gpu->function,
                 gpu->vendor_id, gpu->device_id, gpu->subsys_vendor_id, gpu->subsys_device_id,
                 (unsigned long long)gpu->bar0_base, gpu->is_display_adapter, gpu->has_active_gop, gpu->is_console_display);

        Print(L"[GPU "); PrintDec(i); Print(L"] ");
        PrintColor(EFI_WHITE | EFI_BLACK, gpu->model_name);
        Print(L" ("); Print(gpu->chip_name); Print(L") on Bus ");
        PrintDec(gpu->bus); Print(L":"); PrintDec(gpu->device); Print(L"."); PrintDec(gpu->function);
        Print(L"\r\n        ID: "); Print(gpu->rom_filename + 10);
        Print(L" | BAR0: "); PrintHex32((UINT32)gpu->bar0_base);

        if (gpu->is_console_display) {
            PrintColor(EFI_LIGHTMAGENTA | EFI_BLACK, L" [Active Console Display]\r\n");
        } else if (gpu->has_active_gop) {
            PrintColor(EFI_LIGHTCYAN | EFI_BLACK, L" [Active GOP Display]\r\n");
        } else if (gpu->is_display_adapter) {
            PrintColor(EFI_CYAN | EFI_BLACK, L" [Secondary Video Output]\r\n");
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

        BOOLEAN any_failed = FALSE;
        BOOLEAN unsafe_state = FALSE;

        /*
         * Multi-GPU Ordered Unlock Strategy (3-tier display priority):
         * 1. Pass 1: Unlock headless / secondary GPUs (WITHOUT active video output).
         *    The screen stays completely live and all progress and diagnostics are printed.
         * 2. Pass 2: Unlock the primary display GPU (WITH video output), only if supported!
         *
         * Primary GPU detection priority:
         *   Tier 1: is_console_display (DevicePath matches ConOut — strongest signal)
         *   Tier 2: has_active_gop (GOP protocol with nonzero resolution found)
         *   Tier 3: is_display_adapter (any UEFI driver bound — weakest, most universal)
         *   Fallback: first detected GPU by PCI enumeration order
         */

        /* Determine which GPU index is the primary display adapter (to defer to Pass 2) */
        INTN primary_display_idx = -1;

        /* Tier 1: Console display (strongest — exact match with UEFI ConOut) */
        for (UINTN i = 0; i < g_gpu_count; i++) {
            if (g_gpus[i].is_console_display) {
                primary_display_idx = (INTN)i;
                break;
            }
        }

        /* Tier 2: Active GOP (GOP with active resolution → likely has a monitor) */
        if (primary_display_idx < 0) {
            for (UINTN i = 0; i < g_gpu_count; i++) {
                if (g_gpus[i].has_active_gop) {
                    primary_display_idx = (INTN)i;
                    break;
                }
            }
        }

        /* Tier 3: Display adapter (UEFI driver bound — may or may not have a monitor) */
        if (primary_display_idx < 0) {
            for (UINTN i = 0; i < g_gpu_count; i++) {
                if (g_gpus[i].is_display_adapter) {
                    primary_display_idx = (INTN)i;
                    break;
                }
            }
        }

        /* Ultimate fallback: if nothing detected, first GPU is assumed primary */
        if (primary_display_idx < 0 && g_gpu_count > 0) {
            primary_display_idx = 0;
        }

        /* PASS 1: Unlock all GPUs WITHOUT active display output first */
        for (UINTN i = 0; i < g_gpu_count; i++) {
            if ((INTN)i == primary_display_idx) continue; /* Defer primary display to Pass 2 */
            DETECTED_GPU *gpu = &g_gpus[i];
            if (!gpu->vbios_loaded || !gpu->unlock_selected) continue;

            LogWrite("\n>>> [PASS 1] Unlocking Secondary GPU %u (%ls) Bus %u:%u.%u...\n",
                     (unsigned)i, gpu->model_name, (unsigned)gpu->bus, (unsigned)gpu->device, (unsigned)gpu->function);

            EFI_STATUS u_stat = ExecuteGpuRamUnlock(gpu);
            if (!EFI_ERROR(u_stat) && gpu->unlocked_success) {
                LogWrite("[Unlock] GPU %u unlocked successfully! SS0=0x88888888 SS1=0x8\n", (unsigned)i);
            } else {
                LogWrite("[Unlock] GPU %u unlock failed: status=%#llx (safe_to_return=%u)\n",
                         (unsigned)i, (unsigned long long)u_stat, GuSafeToReturn());
                any_failed = TRUE;

                if (!GuSafeToReturn()) {
                    unsafe_state = TRUE;
                    LogWrite("[FATAL] Hardware safety violation (!GuSafeToReturn) on GPU %u: "
                             "retained=%u allocs=%u dma=%u started=%u quiesced=%u fault=%u\n",
                             (unsigned)i, Gu.retained, Gu.allocations, Gu.dma_count,
                             Gu.started, Gu.quiesced, Gu.fault);
                    PrintColor(EFI_LIGHTRED | EFI_BLACK,
                               L"\r\n[FATAL ERROR] GPU hardware left in an unsafe/unquiesced state (!GuSafeToReturn)!\r\n"
                               L"              Active DMA or retained firmware microcode detected.\r\n"
                               L"              Boot sequence halted immediately to protect host memory.\r\n");
                    break;
                }
            }
        }

        /* PASS 2: Unlock the primary display GPU, only if supported */
        if (!unsafe_state && primary_display_idx >= 0) {
            DETECTED_GPU *gpu = &g_gpus[primary_display_idx];
            if (!gpu->vbios_loaded || !gpu->unlock_selected) {
                LogWrite("\n>>> [PASS 2] Primary Display GPU %u (%ls) not selected/supported for unlock; skipping.\n",
                         (unsigned)primary_display_idx, gpu->model_name);
            } else {
                LogWrite("\n>>> [PASS 2] Unlocking Primary Display GPU %u (%ls) Bus %u:%u.%u...\n",
                         (unsigned)primary_display_idx, gpu->model_name,
                         (unsigned)gpu->bus, (unsigned)gpu->device, (unsigned)gpu->function);

                EFI_STATUS r_stat = ReleaseGpuDisplay(gpu);
                if (EFI_ERROR(r_stat)) {
                    LogWrite("[GOP] GPU %u GOP release failed: status=%#llx\n", (unsigned)primary_display_idx, (unsigned long long)r_stat);
                    any_failed = TRUE;
                } else {
                    EFI_STATUS u_stat = ExecuteGpuRamUnlock(gpu);
                    if (!EFI_ERROR(u_stat) && gpu->unlocked_success) {
                        LogWrite("[Unlock] GPU %u unlocked successfully! SS0=0x88888888 SS1=0x8\n", (unsigned)primary_display_idx);
                    } else {
                        LogWrite("[Unlock] GPU %u unlock failed: status=%#llx (safe_to_return=%u)\n",
                                 (unsigned)primary_display_idx, (unsigned long long)u_stat, GuSafeToReturn());
                        any_failed = TRUE;

                        if (!GuSafeToReturn()) {
                            unsafe_state = TRUE;
                            LogWrite("[FATAL] Hardware safety violation (!GuSafeToReturn) on GPU %u\n", (unsigned)primary_display_idx);
                            PrintColor(EFI_LIGHTRED | EFI_BLACK,
                                       L"\r\n[FATAL ERROR] GPU hardware left in an unsafe/unquiesced state (!GuSafeToReturn)!\r\n");
                        }
                    }

                    /* Always reconnect display GOP and restore console after unlock attempt */
                    ReconnectGpuDisplay(gpu);
                }
            }
        }

        if (unsafe_state) {
            PrintColor(EFI_YELLOW | EFI_BLACK,
                       L"\r\n[CRITICAL HALT] Cannot boot Windows safely in this hardware state.\r\n"
                       L"                Press any key to cold reset system...\r\n");
            LoggerClose();
            EFI_INPUT_KEY rk;
            while (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &rk))) {
                gBS->Stall(100000);
            }
            gRT->ResetSystem(EfiResetCold, EFI_DEVICE_ERROR, 0, NULL);
            for (;;) { }
        }

        if (any_failed) {
            PrintColor(EFI_YELLOW | EFI_BLACK,
                       L"\r\n[WARNING] One or more GPUs failed to unlock cleanly.\r\n"
                       L"          Booting Windows in this state may result in driver Code 43.\r\n\r\n");
            Print(L"Options:\r\n");
            Print(L"  [R] Cold Reset system (Recommended)\r\n");
            Print(L"  [B] Proceed to boot Windows anyway\r\n");
            Print(L"Choice [R/B] (Default [R] in 10s): ");

            EFI_INPUT_KEY fk;
            BOOLEAN boot_anyway = FALSE;
            UINTN fsec = 10;
            while (fsec > 0) {
                EFI_STATUS fks = gST->ConIn->ReadKeyStroke(gST->ConIn, &fk);
                if (!EFI_ERROR(fks)) {
                    if (fk.UnicodeChar == L'b' || fk.UnicodeChar == L'B') {
                        boot_anyway = TRUE;
                        PrintColor(EFI_LIGHTCYAN | EFI_BLACK, L"B (Boot Windows anyway selected)\r\n");
                        break;
                    }
                    if (fk.UnicodeChar == L'r' || fk.UnicodeChar == L'R' || fk.UnicodeChar == 13) {
                        boot_anyway = FALSE;
                        PrintColor(EFI_LIGHTRED | EFI_BLACK, L"R (Cold Reset selected)\r\n");
                        break;
                    }
                }
                gBS->Stall(1000000);
                fsec--;
            }

            if (!boot_anyway) {
                LoggerClose();
                gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
                for (;;) { }
            }
        }

        if (!unsafe_state && !any_failed) {
            PrintColor(EFI_LIGHTCYAN | EFI_BLACK, L"\r\n======================================================================\r\n");
            PrintColor(EFI_LIGHTGREEN | EFI_BLACK, L"              ALL READY GPUS UNLOCKED SUCCESSFULLY!\r\n");
            PrintColor(EFI_LIGHTCYAN | EFI_BLACK, L"======================================================================\r\n");

            for (UINTN i = 0; i < g_gpu_count; i++) {
                DETECTED_GPU *g = &g_gpus[i];
                if (g->unlocked_success) {
                    Print(L"  [GPU "); PrintDec(i); Print(L"] ");
                    PrintColor(EFI_WHITE | EFI_BLACK, g->model_name);
                    Print(L" ("); Print(g->chip_name); Print(L") Bus ");
                    PrintDec(g->bus); Print(L":"); PrintDec(g->device); Print(L"."); PrintDec(g->function);
                    PrintColor(EFI_LIGHTGREEN | EFI_BLACK, L" -> UNLOCKED (SS0=0x88888888, SS1=0x8)\r\n");
                }
            }

            PrintColor(EFI_LIGHTCYAN | EFI_BLACK, L"----------------------------------------------------------------------\r\n");
            PrintColor(EFI_LIGHTGREEN | EFI_BLACK, L"  [+] Display GOP reconnected. Windows will boot with full display.\r\n\r\n");

            /* 10-second countdown with instant skip on any keypress */
            PrintColor(EFI_YELLOW | EFI_BLACK, L"Booting Windows in ");
            UINTN cd = 10;
            while (cd > 0) {
                PrintDec(cd);
                Print(L"... ");

                EFI_INPUT_KEY k;
                EFI_STATUS ks = gST->ConIn ? gST->ConIn->ReadKeyStroke(gST->ConIn, &k) : EFI_NOT_READY;
                if (!EFI_ERROR(ks)) {
                    PrintColor(EFI_LIGHTGREEN | EFI_BLACK, L"[Boot Now]\r\n");
                    break;
                }

                gBS->Stall(1000000);
                cd--;
            }
            if (cd == 0) {
                Print(L"0\r\n");
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
