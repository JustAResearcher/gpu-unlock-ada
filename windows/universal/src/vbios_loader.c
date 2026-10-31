/* SPDX-License-Identifier: GPL-2.0-only */
#include "vbios_loader.h"
#include "logger.h"
#include "uefi_base.h"
#include "runtime.h"

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
 * Searches for a valid PCI Option ROM inside the buffer matching expected Device ID:
 * 1. Offset 0 (standard GPU-Z dump starting with 0x55 0xAA)
 * 2. Scans 512-byte boundaries (nvflash EEPROM dumps where VBIOS begins after NVGI container)
 */
static BOOLEAN LocateOptionRom(VOID *raw_buf, UINTN total_size, UINT16 expected_devid, VOID **rom_start, UINTN *rom_size)
{
    const UINT8 *bytes = (const UINT8 *)raw_buf;

    for (UINTN pos = 0; pos + 512 <= total_size; pos += 512) {
        if (bytes[pos] == 0x55 && bytes[pos + 1] == 0xAA) {
            /* Check if PCIR header follows at pos + 0x18 */
            u16 pcir_rel = Read16(bytes + pos + 0x18);
            if (pos + pcir_rel + 8 <= total_size) {
                u32 pcir_sig = Read32(bytes + pos + pcir_rel);
                if (pcir_sig == 0x52494350) { /* 'PCIR' */
                    u16 ven = Read16(bytes + pos + pcir_rel + 4);
                    u16 dev = Read16(bytes + pos + pcir_rel + 6);
                    if (ven == 0x10de && dev == expected_devid) {
                        *rom_start = (VOID *)(bytes + pos);
                        *rom_size = total_size - pos;
                        LogWrite("[VBIOS] Located valid PCIR Option ROM at offset %#x (effective size=%u, DevID=%#x)\n",
                                 (unsigned)pos, (unsigned)(*rom_size), dev);
                        return TRUE;
                    } else if (ven == 0x10de) {
                        LogWrite("[VBIOS] Found Option ROM at offset %#x with mismatched DevID (%#x != %#x)\n",
                                 (unsigned)pos, dev, expected_devid);
                    }
                }
            }
        }
    }
    return FALSE;
}

/**
 * Extracts authentic 1MB factory VBIOS directly from the GPU's hardware BAR0 PROM aperture (0x300000).
 * Used when no USB file exists or when the USB file has an ID mismatch (e.g. secondary GPU).
 */
static BOOLEAN ExtractVbiosFromHardwareBar0(DETECTED_GPU *gpu, EFI_FILE_HANDLE root)
{
    if (!gpu || !gpu->pci_io || !gpu->bar0_base) return FALSE;

    /* 1. Ensure Memory Space decode is active in PCI Command */
    UINT16 orig_cmd = 0;
    EFI_STATUS s = gpu->pci_io->Pci.Read(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &orig_cmd);
    if (EFI_ERROR(s)) return FALSE;

    UINT16 active_cmd = (orig_cmd & ~0x0004) | 0x0003;
    gpu->pci_io->Pci.Write(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &active_cmd);

    /* 2. Read first bytes from BAR0+0x300000 */
    UINT8 sig[2] = {0};
    s = gpu->pci_io->Mem.Read(gpu->pci_io, EfiPciIoWidthUint8, 0, 0x300000, 2, sig);
    if (EFI_ERROR(s) || sig[0] != 0x55 || sig[1] != 0xAA) {
        LogWrite("[VBIOS] Hardware BAR0+0x300000 does not start with 0x55 0xAA (found: %#x %#x)\n",
                 sig[0], sig[1]);
        gpu->pci_io->Pci.Write(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &orig_cmd);
        return FALSE;
    }

    /* 3. Allocate 1MB buffer and copy hardware PROM */
    UINTN prom_size = 1024 * 1024;
    VOID *prom_buf = NULL;
    s = gBS->AllocatePool(EfiBootServicesData, prom_size, &prom_buf);
    if (EFI_ERROR(s) || !prom_buf) {
        gpu->pci_io->Pci.Write(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &orig_cmd);
        return FALSE;
    }

    UINTN done = 0;
    while (done < prom_size) {
        UINTN chunk = (prom_size - done < 4096) ? (prom_size - done) : 4096;
        s = gpu->pci_io->Mem.Read(gpu->pci_io, EfiPciIoWidthUint8, 0, 0x300000 + done, chunk, (UINT8 *)prom_buf + done);
        if (EFI_ERROR(s)) {
            LogWrite("[VBIOS] Hardware PROM read error at offset %#x\n", (unsigned)(0x300000 + done));
            gBS->FreePool(prom_buf);
            gpu->pci_io->Pci.Write(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &orig_cmd);
            return FALSE;
        }
        done += chunk;
    }
    gpu->pci_io->Pci.Write(gpu->pci_io, EfiPciIoWidthUint16, 4, 1, &orig_cmd);

    /* 4. Validate PCIR header */
    const UINT8 *bytes = (const UINT8 *)prom_buf;
    UINT16 pcir_rel = Read16(bytes + 0x18);
    if ((UINTN)pcir_rel + 8 > prom_size) {
        gBS->FreePool(prom_buf);
        return FALSE;
    }

    const UINT8 *pcir = bytes + pcir_rel;
    if (pcir[0] != 'P' || pcir[1] != 'C' || pcir[2] != 'I' || pcir[3] != 'R') {
        LogWrite("[VBIOS] Hardware BAR0 PROM missing PCIR signature\n");
        gBS->FreePool(prom_buf);
        return FALSE;
    }

    UINT16 ven = Read16(pcir + 4);
    UINT16 dev = Read16(pcir + 6);
    if (ven != 0x10de || dev != gpu->device_id) {
        LogWrite("[VBIOS] Hardware BAR0 PROM has unexpected ID: %#x:%#x (expected %#x:%#x)\n",
                 ven, dev, gpu->vendor_id, gpu->device_id);
        gBS->FreePool(prom_buf);
        return FALSE;
    }

    gpu->vbios_data = prom_buf;
    gpu->vbios_size = prom_size;
    gpu->vbios_loaded = TRUE;
    LogWrite("[VBIOS] Successfully extracted 1MB authentic VBIOS from hardware BAR0 (DevID=%#x)\n", dev);

    /* 5. Auto-save to USB drive so future boots load directly from file */
    if (root && gpu->rom_filename[0] != L'\0') {
        EFI_FILE_HANDLE out_f = NULL;
        s = root->Open(root, &out_f, gpu->rom_filename,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
        if (!EFI_ERROR(s) && out_f) {
            UINTN wb = prom_size;
            out_f->Write(out_f, &wb, prom_buf);
            out_f->Close(out_f);
            LogWrite("[VBIOS] Automatically updated %ls on USB drive with authentic hardware dump\n", gpu->rom_filename);
        }
    }

    return TRUE;
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
        if (ExtractVbiosFromHardwareBar0(target_gpu, NULL)) return EFI_SUCCESS;
        return EFI_NOT_FOUND;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    status = gBS->HandleProtocol(loaded_image->DeviceHandle, &SimpleFileSystemProtocolGuid, (VOID **)&fs);
    if (EFI_ERROR(status) || !fs) {
        LogWrite("[VBIOS] Failed to get SimpleFileSystem protocol (%#llx)\n", (unsigned long long)status);
        if (ExtractVbiosFromHardwareBar0(target_gpu, NULL)) return EFI_SUCCESS;
        return EFI_NOT_FOUND;
    }

    EFI_FILE_HANDLE root = NULL;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status) || !root) {
        LogWrite("[VBIOS] Failed to open filesystem volume (%#llx)\n", (unsigned long long)status);
        if (ExtractVbiosFromHardwareBar0(target_gpu, NULL)) return EFI_SUCCESS;
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
        LogWrite("[VBIOS] File %ls not found on boot drive; attempting hardware BAR0 extraction...\n", target_gpu->rom_filename);
        if (ExtractVbiosFromHardwareBar0(target_gpu, root)) {
            root->Close(root);
            return EFI_SUCCESS;
        }
        root->Close(root);
        return EFI_NOT_FOUND;
    }

    VOID *raw_buf = NULL;
    UINTN raw_size = 0;
    status = ReadFileContent(file, &raw_buf, &raw_size);
    file->Close(file);

    if (EFI_ERROR(status)) {
        LogWrite("[VBIOS] Read error: %#llx; attempting hardware BAR0 extraction...\n", (unsigned long long)status);
        if (ExtractVbiosFromHardwareBar0(target_gpu, root)) {
            root->Close(root);
            return EFI_SUCCESS;
        }
        root->Close(root);
        return status;
    }

    /* Locate Option ROM inside the loaded data */
    VOID *rom_start = NULL;
    UINTN rom_size = 0;
    if (!LocateOptionRom(raw_buf, raw_size, target_gpu->device_id, &rom_start, &rom_size)) {
        LogWrite("[VBIOS] Warning: USB file does not match DevID %#x; extracting authentic VBIOS from hardware BAR0...\n",
                 target_gpu->device_id);
        gBS->FreePool(raw_buf);
        if (ExtractVbiosFromHardwareBar0(target_gpu, root)) {
            root->Close(root);
            return EFI_SUCCESS;
        }
        root->Close(root);
        return EFI_COMPROMISED_DATA;
    }

    root->Close(root);
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
