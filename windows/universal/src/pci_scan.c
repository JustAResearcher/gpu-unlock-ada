/* SPDX-License-Identifier: GPL-2.0-only */
#include "pci_scan.h"
#include "uefi_base.h"

static EFI_GUID PciIoProtocolGuid = EFI_PCI_IO_PROTOCOL_GUID;

GPU_ARCH ClassifyGpuArchitecture(UINT16 device_id, CONST CHAR16 **chip_name, CONST CHAR16 **model_name)
{
    /* Ampere (RTX 30-Series) Device ID ranges */
    if (device_id >= 0x2200 && device_id <= 0x223F) {
        if (chip_name)  *chip_name  = L"GA102";
        if (model_name) *model_name = L"RTX 3080/3080Ti/3090";
        return GPU_ARCH_AMPERE;
    }
    if (device_id >= 0x2414 && device_id <= 0x243F) {
        if (chip_name)  *chip_name  = L"GA103";
        if (model_name) *model_name = L"RTX 3060Ti/3080Ti-M";
        return GPU_ARCH_AMPERE;
    }
    if (device_id >= 0x2480 && device_id <= 0x24BF) {
        if (chip_name)  *chip_name  = L"GA104";
        if (model_name) *model_name = L"RTX 3070/3070Ti";
        return GPU_ARCH_AMPERE;
    }
    if (device_id >= 0x2500 && device_id <= 0x257F) {
        if (chip_name)  *chip_name  = L"GA106";
        if (model_name) *model_name = L"RTX 3060";
        return GPU_ARCH_AMPERE;
    }
    if (device_id >= 0x2580 && device_id <= 0x25FF) {
        if (chip_name)  *chip_name  = L"GA107";
        if (model_name) *model_name = L"RTX 3050";
        return GPU_ARCH_AMPERE;
    }

    /* Ada Lovelace (RTX 40-Series) Device ID ranges */
    if (device_id >= 0x2680 && device_id <= 0x26BF) {
        if (chip_name)  *chip_name  = L"AD102";
        if (model_name) *model_name = L"RTX 4090/6000Ada";
        return GPU_ARCH_ADA_LOVELACE;
    }
    if (device_id >= 0x2700 && device_id <= 0x277F) {
        if (chip_name)  *chip_name  = L"AD103";
        if (model_name) *model_name = L"RTX 4080/4080S";
        return GPU_ARCH_ADA_LOVELACE;
    }
    if (device_id >= 0x2780 && device_id <= 0x27FF) {
        if (chip_name)  *chip_name  = L"AD104";
        if (model_name) *model_name = L"RTX 4070/4070Ti";
        return GPU_ARCH_ADA_LOVELACE;
    }
    if (device_id >= 0x2800 && device_id <= 0x287F) {
        if (chip_name)  *chip_name  = L"AD106";
        if (model_name) *model_name = L"RTX 4060Ti";
        return GPU_ARCH_ADA_LOVELACE;
    }
    if (device_id >= 0x2880 && device_id <= 0x28FF) {
        if (chip_name)  *chip_name  = L"AD107";
        if (model_name) *model_name = L"RTX 4060";
        return GPU_ARCH_ADA_LOVELACE;
    }

    if (chip_name)  *chip_name  = L"UNKNOWN";
    if (model_name) *model_name = L"Unsupported GPU";
    return GPU_ARCH_UNKNOWN;
}

static VOID HexToWString(UINT16 val, CHAR16 *buf)
{
    static const CHAR16 hex[] = L"0123456789abcdef";
    buf[0] = hex[(val >> 12) & 0xF];
    buf[1] = hex[(val >> 8) & 0xF];
    buf[2] = hex[(val >> 4) & 0xF];
    buf[3] = hex[val & 0xF];
    buf[4] = L'\0';
}

VOID FormatGpuRomFileName(CONST DETECTED_GPU *gpu, CHAR16 *out_path, UINTN max_chars)
{
    /* Format strictly: \EFI\VBIOS\<vendor>_<device>_<subvendor>_<subdevice>.rom */
    CHAR16 v[5], d[5], sv[5], sd[5];
    HexToWString(gpu->vendor_id, v);
    HexToWString(gpu->device_id, d);
    HexToWString(gpu->subsys_vendor_id, sv);
    HexToWString(gpu->subsys_device_id, sd);

    /* Construct: \EFI\VBIOS\vvvv_dddd_ssss_dddd.rom */
    UINTN idx = 0;
    CONST CHAR16 *prefix = L"\\EFI\\VBIOS\\";
    while (*prefix && idx < max_chars - 1) out_path[idx++] = *prefix++;
    
    for (int i = 0; i < 4 && idx < max_chars - 1; i++) out_path[idx++] = v[i];
    if (idx < max_chars - 1) out_path[idx++] = L'_';
    for (int i = 0; i < 4 && idx < max_chars - 1; i++) out_path[idx++] = d[i];
    if (idx < max_chars - 1) out_path[idx++] = L'_';
    for (int i = 0; i < 4 && idx < max_chars - 1; i++) out_path[idx++] = sv[i];
    if (idx < max_chars - 1) out_path[idx++] = L'_';
    for (int i = 0; i < 4 && idx < max_chars - 1; i++) out_path[idx++] = sd[i];
    
    CONST CHAR16 *ext = L".rom";
    while (*ext && idx < max_chars - 1) out_path[idx++] = *ext++;
    out_path[idx] = L'\0';
}

static EFI_GUID DevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

static BOOLEAN CompareBytes(CONST VOID *a, CONST VOID *b, UINTN len)
{
    CONST UINT8 *p1 = (CONST UINT8 *)a;
    CONST UINT8 *p2 = (CONST UINT8 *)b;
    for (UINTN i = 0; i < len; i++) {
        if (p1[i] != p2[i]) return FALSE;
    }
    return TRUE;
}

static UINTN GetDevicePathLengthExcludingEnd(CONST EFI_DEVICE_PATH_PROTOCOL *dp)
{
    if (!dp) return 0;
    CONST EFI_DEVICE_PATH_PROTOCOL *curr = dp;
    UINTN size = 0;
    /* 0x7F = End of Hardware Device Path, 0xFF = End Entire Device Path */
    while (!(curr->Type == 0x7F && curr->SubType == 0xFF)) {
        UINT16 len = (UINT16)curr->Length[0] | ((UINT16)curr->Length[1] << 8);
        if (len < 4) break;
        size += len;
        curr = (CONST EFI_DEVICE_PATH_PROTOCOL *)((CONST UINT8 *)curr + len);
    }
    return size;
}

static BOOLEAN CheckIfConsoleDisplay(EFI_HANDLE gpu_handle)
{
    if (!gpu_handle || !gST || !gST->ConsoleOutHandle) return FALSE;

    EFI_DEVICE_PATH_PROTOCOL *gpu_dp = NULL;
    EFI_STATUS s = gBS->HandleProtocol(gpu_handle, &DevicePathProtocolGuid, (VOID **)&gpu_dp);
    if (EFI_ERROR(s) || !gpu_dp) return FALSE;

    EFI_DEVICE_PATH_PROTOCOL *con_dp = NULL;
    s = gBS->HandleProtocol(gST->ConsoleOutHandle, &DevicePathProtocolGuid, (VOID **)&con_dp);
    if (EFI_ERROR(s) || !con_dp) return FALSE;

    UINTN gpu_len = GetDevicePathLengthExcludingEnd(gpu_dp);
    UINTN con_len = GetDevicePathLengthExcludingEnd(con_dp);
    if (gpu_len > 0 && con_len >= gpu_len) {
        if (CompareBytes(gpu_dp, con_dp, gpu_len)) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN CheckIfDisplayAdapter(EFI_HANDLE handle, BOOLEAN *out_is_console)
{
    BOOLEAN is_con = CheckIfConsoleDisplay(handle);
    if (out_is_console) *out_is_console = is_con;
    if (is_con) return TRUE;

    if (!handle) return FALSE;

    /* 1. Direct GOP on GPU controller handle */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status = gBS->HandleProtocol(handle, &GopGuid, (VOID **)&gop);
    if (!EFI_ERROR(status) && gop && gop->Mode && gop->Mode->Info) {
        if (gop->Mode->Info->HorizontalResolution > 0 && gop->Mode->Info->VerticalResolution > 0) {
            return TRUE;
        }
    }

    /* 2. Check child controller handles produced by GOP driver */
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries = NULL;
    UINTN count = 0;
    status = gBS->OpenProtocolInformation(handle, &PciIoProtocolGuid, &entries, &count);
    if (!EFI_ERROR(status) && entries) {
        BOOLEAN has_display = FALSE;
        for (UINTN i = 0; i < count; i++) {
            if (entries[i].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) {
                EFI_HANDLE child = entries[i].ControllerHandle;
                if (child) {
                    gop = NULL;
                    status = gBS->HandleProtocol(child, &GopGuid, (VOID **)&gop);
                    if (!EFI_ERROR(status) && gop && gop->Mode && gop->Mode->Info) {
                        if (gop->Mode->Info->HorizontalResolution > 0 && gop->Mode->Info->VerticalResolution > 0) {
                            has_display = TRUE;
                            break;
                        }
                    }
                }
            }
        }
        gBS->FreePool(entries);
        if (has_display) return TRUE;
    }

    return FALSE;
}

UINTN ScanSystemGpus(DETECTED_GPU out_gpus[], UINTN max_gpus)
{
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    UINTN detected = 0;

    EFI_STATUS status = gBS->LocateHandleBuffer(ByProtocol, &PciIoProtocolGuid, NULL, &count, &handles);
    if (EFI_ERROR(status) || !handles || count == 0) {
        return 0;
    }

    for (UINTN i = 0; i < count && detected < max_gpus; i++) {
        EFI_PCI_IO_PROTOCOL *pci = NULL;
        status = gBS->HandleProtocol(handles[i], &PciIoProtocolGuid, (VOID **)&pci);
        if (EFI_ERROR(status) || !pci || !pci->Pci.Read) {
            continue;
        }

        UINT32 id_reg = 0;
        status = pci->Pci.Read(pci, EfiPciIoWidthUint32, 0, 1, &id_reg);
        if (EFI_ERROR(status)) continue;

        UINT16 vendor = (UINT16)(id_reg & 0xFFFF);
        UINT16 device = (UINT16)(id_reg >> 16);

        if (vendor != NVIDIA_VENDOR_ID) {
            continue;
        }

        CONST CHAR16 *chip = NULL;
        CONST CHAR16 *model = NULL;
        GPU_ARCH arch = ClassifyGpuArchitecture(device, &chip, &model);
        if (arch == GPU_ARCH_UNKNOWN) {
            /* Unsupported GPU generation (e.g. Pascal, Turing, Volta) */
            continue;
        }

        /* Read full PCI Configuration Header */
        UINT32 cfg[64];
        status = pci->Pci.Read(pci, EfiPciIoWidthUint32, 0, 64, cfg);
        if (EFI_ERROR(status)) continue;

        UINT32 class_rev = cfg[2];
        UINT32 class_code = class_rev >> 8;
        /* Verify VGA display or 3D compute controller */
        if ((class_code & 0xFFFF00) != 0x030000 && (class_code & 0xFFFF00) != 0x030200) {
            continue;
        }

        UINT32 subsys = cfg[11];
        UINT16 sub_vendor = (UINT16)(subsys & 0xFFFF);
        UINT16 sub_device = (UINT16)(subsys >> 16);

        UINTN seg, bus, dev, fun;
        if (pci->GetLocation) {
            pci->GetLocation(pci, &seg, &bus, &dev, &fun);
        } else {
            seg = bus = dev = fun = 0;
        }

        /* Determine BAR0 physical address */
        UINT64 bar0 = (UINT64)(cfg[4] & ~0xFULL);
        if ((cfg[4] & 0x6) == 0x4) { /* 64-bit BAR */
            bar0 |= ((UINT64)cfg[5] << 32);
        }

        DETECTED_GPU *gpu = &out_gpus[detected];
        gpu->vendor_id = vendor;
        gpu->device_id = device;
        gpu->subsys_vendor_id = sub_vendor;
        gpu->subsys_device_id = sub_device;
        gpu->revision_id = (UINT8)(class_rev & 0xFF);
        gpu->class_code = class_code;
        gpu->segment = seg;
        gpu->bus = bus;
        gpu->device = dev;
        gpu->function = fun;
        gpu->bar0_base = bar0;
        gpu->bar0_size = 0x1000000; /* Standard 16 MiB BAR0 */
        gpu->arch = arch;
        gpu->chip_name = chip;
        gpu->model_name = model;
        gpu->handle = handles[i];
        gpu->pci_io = pci;
        BOOLEAN is_con = FALSE;
        gpu->is_display_adapter = CheckIfDisplayAdapter(handles[i], &is_con);
        gpu->is_console_display = is_con;
        gpu->vbios_data = NULL;
        gpu->vbios_size = 0;
        gpu->vbios_loaded = FALSE;
        gpu->unlock_selected = TRUE;
        gpu->unlocked_success = FALSE;

        FormatGpuRomFileName(gpu, gpu->rom_filename, ARRAY_SIZE(gpu->rom_filename));
        detected++;
    }

    if (handles) {
        gBS->FreePool(handles);
    }
    return detected;
}

