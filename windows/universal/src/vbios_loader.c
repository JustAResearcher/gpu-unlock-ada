/* SPDX-License-Identifier: GPL-2.0-only */
#include "vbios_loader.h"
#include "logger.h"
#include "uefi_base.h"

static EFI_GUID LoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID SimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID FileInfoGuid = EFI_FILE_INFO_ID;

static inline u16 Read16(const u8 *p)
{
    return (u16)p[0] | ((u16)p[1] << 8);
}

static inline u32 Read32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static VOID StringToUpper(CONST CHAR16 *src, CHAR16 *dst, UINTN max_chars)
{
    UINTN i = 0;
    while (src[i] && i < max_chars - 1) {
        if (src[i] >= L'a' && src[i] <= L'z') {
            dst[i] = src[i] - (L'a' - L'A');
        } else {
            dst[i] = src[i];
        }
        i++;
    }
    dst[i] = L'\0';
}

static EFI_STATUS TryOpenFile(EFI_FILE_HANDLE root, CONST CHAR16 *path, EFI_FILE_HANDLE *out_file)
{
    EFI_FILE_HANDLE f = NULL;
    EFI_STATUS s = root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(s) && f) {
        *out_file = f;
        return EFI_SUCCESS;
    }
    return s;
}

static EFI_STATUS ReadFileContent(EFI_FILE_HANDLE file, VOID **out_buf, UINTN *out_size)
{
    UINT8 info_buf[sizeof(EFI_FILE_INFO) + 256];
    UINTN info_size = sizeof(info_buf);
    EFI_STATUS status = file->GetInfo(file, &FileInfoGuid, &info_size, info_buf);
    if (EFI_ERROR(status)) return status;

    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
    UINTN file_size = (UINTN)info->FileSize;

    /* VBIOS size sanity: between 64 KiB and 8 MiB */
    if (file_size < 64 * 1024 || file_size > 8 * 1024 * 1024) {
        LogWrite("[VBIOS] Bad file size: %u bytes\n", (unsigned)file_size);
        return EFI_BAD_BUFFER_SIZE;
    }

    VOID *data = NULL;
    status = gBS->AllocatePool(EfiBootServicesData, file_size, &data);
    if (EFI_ERROR(status) || !data) return EFI_OUT_OF_RESOURCES;

    UINTN read_bytes = file_size;
    status = file->Read(file, &read_bytes, data);
    if (EFI_ERROR(status) || read_bytes != file_size) {
        gBS->FreePool(data);
        return EFI_DEVICE_ERROR;
    }

    *out_buf = data;
    *out_size = file_size;
    return EFI_SUCCESS;
}

/**
 * Searches for a valid PCI Option ROM inside the buffer:
 * 1. Offset 0 (standard GPU-Z dump starting with 0x55 0xAA)
 * 2. Scans 512-byte boundaries (nvflash EEPROM dumps where VBIOS begins after NVGI container)
 */
static BOOLEAN LocateOptionRom(VOID *raw_buf, UINTN total_size, VOID **rom_start, UINTN *rom_size)
{
    const UINT8 *bytes = (const UINT8 *)raw_buf;

    for (UINTN pos = 0; pos + 512 <= total_size; pos += 512) {
        if (bytes[pos] == 0x55 && bytes[pos + 1] == 0xAA) {
            /* Check if PCIR header follows at pos + 0x18 */
            u16 pcir_rel = Read16(bytes + pos + 0x18);
            if (pos + pcir_rel + 4 <= total_size) {
                u32 pcir_sig = Read32(bytes + pos + pcir_rel);
                if (pcir_sig == 0x52494350) { /* 'PCIR' */
                    *rom_start = (VOID *)(bytes + pos);
                    *rom_size = total_size - pos;
                    LogWrite("[VBIOS] Located valid PCIR Option ROM at offset %#x (effective size=%u)\n",
                             (unsigned)pos, (unsigned)(*rom_size));
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

EFI_STATUS LoadGpuVbiosStrictId(DETECTED_GPU *target_gpu, CONST DETECTED_GPU all_gpus[], UINTN gpu_count)
{
    if (!target_gpu) return EFI_INVALID_PARAMETER;

    /* Check if an identical sibling card already loaded this exact ROM in RAM */
    for (UINTN j = 0; j < gpu_count; j++) {
        CONST DETECTED_GPU *sibling = &all_gpus[j];
        if (sibling != target_gpu && sibling->vbios_loaded && sibling->vbios_data) {
            if (sibling->vendor_id == target_gpu->vendor_id &&
                sibling->device_id == target_gpu->device_id &&
                sibling->subsys_vendor_id == target_gpu->subsys_vendor_id &&
                sibling->subsys_device_id == target_gpu->subsys_device_id) {
                target_gpu->vbios_data = sibling->vbios_data;
                target_gpu->vbios_size = sibling->vbios_size;
                target_gpu->vbios_loaded = TRUE;
                LogWrite("[VBIOS] Reusing RAM VBIOS buffer from identical sibling card (GPU %u)\n", (unsigned)j);
                return EFI_SUCCESS;
            }
        }
    }

    /* Locate boot file system (the partition we booted from) */
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_STATUS status = gBS->HandleProtocol(gImageHandle, &LoadedImageProtocolGuid, (VOID **)&loaded_image);
    if (EFI_ERROR(status) || !loaded_image || !loaded_image->DeviceHandle) {
        LogWrite("[VBIOS] Failed to get LoadedImage protocol (%#llx)\n", (unsigned long long)status);
        return EFI_NOT_FOUND;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    status = gBS->HandleProtocol(loaded_image->DeviceHandle, &SimpleFileSystemProtocolGuid, (VOID **)&fs);
    if (EFI_ERROR(status) || !fs) {
        LogWrite("[VBIOS] Failed to get SimpleFileSystem protocol (%#llx)\n", (unsigned long long)status);
        return EFI_NOT_FOUND;
    }

    EFI_FILE_HANDLE root = NULL;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status) || !root) {
        LogWrite("[VBIOS] Failed to open filesystem volume (%#llx)\n", (unsigned long long)status);
        return EFI_NOT_FOUND;
    }

    /* Candidate path variants (with/without leading backslash, lowercase/uppercase) */
    CHAR16 upper_path[64];
    StringToUpper(target_gpu->rom_filename, upper_path, ARRAY_SIZE(upper_path));

    CONST CHAR16 *candidates[8];
    UINTN num_candidates = 0;

    candidates[num_candidates++] = target_gpu->rom_filename;           /* \EFI\VBIOS\10de_...rom */
    candidates[num_candidates++] = target_gpu->rom_filename + 1;       /* EFI\VBIOS\10de_...rom */
    candidates[num_candidates++] = upper_path;                          /* \EFI\VBIOS\10DE_...ROM */
    candidates[num_candidates++] = upper_path + 1;                      /* EFI\VBIOS\10DE_...ROM */

    EFI_FILE_HANDLE file = NULL;
    for (UINTN i = 0; i < num_candidates; i++) {
        status = TryOpenFile(root, candidates[i], &file);
        if (!EFI_ERROR(status) && file) {
            LogWrite("[VBIOS] Opened ROM candidate: %ls\n", candidates[i]);
            break;
        }
    }

    /* If not found by full path, try traversing directory \EFI\VBIOS explicitly */
    if (!file) {
        EFI_FILE_HANDLE dir = NULL;
        status = root->Open(root, &dir, L"\\EFI\\VBIOS", EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(status) || !dir) {
            root->Open(root, &dir, L"EFI\\VBIOS", EFI_FILE_MODE_READ, 0);
        }
        if (dir) {
            /* Try bare filename inside \EFI\VBIOS */
            CONST CHAR16 *bare_name = target_gpu->rom_filename + 11; /* Skip \EFI\VBIOS\ */
            CONST CHAR16 *bare_upper = upper_path + 11;
            status = TryOpenFile(dir, bare_name, &file);
            if (EFI_ERROR(status)) {
                status = TryOpenFile(dir, bare_upper, &file);
            }
            dir->Close(dir);
            if (!EFI_ERROR(status) && file) {
                LogWrite("[VBIOS] Found inside \\EFI\\VBIOS: %ls\n", bare_name);
            }
        }
    }

    if (!file) {
        root->Close(root);
        LogWrite("[VBIOS] File %ls not found on boot drive\n", target_gpu->rom_filename);
        return EFI_NOT_FOUND;
    }

    VOID *raw_buf = NULL;
    UINTN raw_size = 0;
    status = ReadFileContent(file, &raw_buf, &raw_size);
    file->Close(file);
    root->Close(root);

    if (EFI_ERROR(status)) {
        LogWrite("[VBIOS] Read error: %#llx\n", (unsigned long long)status);
        return status;
    }

    /* Locate Option ROM inside the loaded data */
    VOID *rom_start = NULL;
    UINTN rom_size = 0;
    if (!LocateOptionRom(raw_buf, raw_size, &rom_start, &rom_size)) {
        LogWrite("[VBIOS] Error: File does not contain a valid Option ROM with PCIR signature\n");
        gBS->FreePool(raw_buf);
        return EFI_COMPROMISED_DATA;
    }

    target_gpu->vbios_data = rom_start;
    target_gpu->vbios_size = rom_size;
    target_gpu->vbios_loaded = TRUE;
    LogWrite("[VBIOS] Successfully prepared VBIOS in RAM (%u bytes)\n", (unsigned)rom_size);

    return EFI_SUCCESS;
}

VOID FreeGpuVbios(DETECTED_GPU *gpu)
{
    if (gpu && gpu->vbios_loaded && gpu->vbios_data) {
        gBS->FreePool(gpu->vbios_data);
        gpu->vbios_data = NULL;
        gpu->vbios_size = 0;
        gpu->vbios_loaded = FALSE;
    }
}
