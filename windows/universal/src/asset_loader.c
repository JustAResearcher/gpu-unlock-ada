/* SPDX-License-Identifier: GPL-2.0-only */
#include "asset_loader.h"
#include "logger.h"
#include "port.h"
#include "profile.h"
#include "compat/compat.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

static EFI_GUID LoadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID FsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID InfoGuid = EFI_FILE_INFO_ID;

typedef struct {
    const CHAR16 *file_name;
    const char   *registry_name;
    UINTN         size;
    BOOLEAN       required;
} ASSET_DESCRIPTOR;

static const ASSET_DESCRIPTOR g_all_assets[] = {
    /* Shared GSP-RM firmware (Ampere + Ada) */
    { L"gsp_ga10x.bin", "gsp_ga10x.bin", 84277400, TRUE },

    /* Ada Lovelace (RTX 40 Series) */
    { L"booter_load_ad102.bin", "booter_load_ad102.bin", 57672, FALSE },
    { L"v67-ad102-610.43.03-four-plm.bin", "v67-ad102-610.43.03-four-plm.bin", 64000, FALSE },
    { L"gsp_boot_ad102.bin", "gsp_boot_ad102.bin", 36864, FALSE },
    { L"gsp_boot_desc_ad102.bin", "gsp_boot_desc_ad102.bin", 84, FALSE },

    /* Ampere (RTX 30 Series, including RTX 3060) */
    { L"booter_load_ga102.bin", "booter_load_ga102.bin", 61256, FALSE },
    { L"v67-ga102-610.43.03.bin", "v67-ga102-610.43.03.bin", 64000, FALSE },
    { L"gsp_boot_ga102.bin", "gsp_boot_ga102.bin", 24576, FALSE },
    { L"gsp_boot_desc_ga102.bin", "gsp_boot_desc_ga102.bin", 84, FALSE }
};

#define TOTAL_KNOWN_ASSETS (sizeof(g_all_assets) / sizeof(g_all_assets[0]))

static EFI_STATUS ReadAssetFile(EFI_FILE_HANDLE root, const CHAR16 *dir_name, const CHAR16 *file_name, UINTN expected, VOID **data)
{
    *data = NULL;
    EFI_FILE_HANDLE f = NULL;
    CHAR16 path[128];
    UINTN pos = 0;

    for (UINTN i = 0; dir_name[i] != 0 && pos < 120; i++) path[pos++] = dir_name[i];
    for (UINTN i = 0; file_name[i] != 0 && pos < 127; i++) path[pos++] = file_name[i];
    path[pos] = 0;

    EFI_STATUS s = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s) || !f) return s;

    UINT64 info_words[128];
    UINTN info_bytes = sizeof(info_words);
    s = f->GetInfo(f, &InfoGuid, &info_bytes, info_words);
    EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_words;
    if (EFI_ERROR(s) || info->FileSize != expected || (info->Attribute & EFI_FILE_DIRECTORY)) {
        LogWrite("[Assets] Size mismatch '%ls': got %llu, expected %llu\n",
                 path, (unsigned long long)info->FileSize, (unsigned long long)expected);
        f->Close(f);
        return EFI_COMPROMISED_DATA;
    }

    VOID *p = NULL;
    UINTN pages = (expected + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS addr = 0;
    s = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
    if (EFI_ERROR(s) || !addr) {
        LogWrite("[Assets] AllocatePages failed for '%ls' (%u bytes, status=%#llx)\n",
                 path, (unsigned)expected, (unsigned long long)s);
        f->Close(f);
        return EFI_OUT_OF_RESOURCES;
    }
    p = (VOID *)(UINTN)addr;
    memset(p, 0, expected);

    for (UINTN at = 0; at < expected;) {
        UINTN n = expected - at;
        if (n > 1048576) n = 1048576;
        UINTN wanted = n;
        s = f->Read(f, &n, (UINT8 *)p + at);
        if (EFI_ERROR(s) || n != wanted) {
            LogWrite("[Assets] Read error on '%ls' at %u (read %u of %u, status=%#llx)\n",
                     path, (unsigned)at, (unsigned)n, (unsigned)wanted, (unsigned long long)s);
            f->Close(f);
            gBS->FreePages(addr, pages);
            return EFI_DEVICE_ERROR;
        }
        at += n;
    }

    f->Close(f);
    *data = p;
    LogWrite("[Assets] Loaded '%ls' (%u bytes)\n", path, (unsigned)expected);
    return EFI_SUCCESS;
}

EFI_STATUS LoadFirmwareAssets(EFI_HANDLE image_handle)
{
    static const CHAR16 *search_dirs[] = {
        L"\\EFI\\ASSETS\\",
        L"EFI\\ASSETS\\",
        L"\\UNIVERSAL-ASSETS\\",
        L"UNIVERSAL-ASSETS\\",
        L"\\ASSETS\\",
        L"ASSETS\\"
    };
    const UINTN num_dirs = sizeof(search_dirs) / sizeof(search_dirs[0]);

    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    EFI_STATUS s = gBS->HandleProtocol(image_handle, &LoadedGuid, (VOID **)&loaded);
    if (EFI_ERROR(s) || !loaded || !loaded->DeviceHandle) return s;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    s = gBS->HandleProtocol(loaded->DeviceHandle, &FsGuid, (VOID **)&fs);
    if (EFI_ERROR(s) || !fs) return s;

    EFI_FILE_HANDLE root = NULL;
    s = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(s) || !root) return s;

    GuAssetCount = 0;
    BOOLEAN ada_loaded = FALSE;
    BOOLEAN ampere_loaded = FALSE;
    BOOLEAN gsp_loaded = FALSE;

    /* Scan each search directory for matching firmware files */
    for (UINTN d = 0; d < num_dirs; d++) {
        const CHAR16 *dir = search_dirs[d];

        for (UINTN i = 0; i < TOTAL_KNOWN_ASSETS; i++) {
            /* Skip if already loaded in an earlier directory */
            BOOLEAN already = FALSE;
            for (UINTN a = 0; a < GuAssetCount; a++) {
                if (!strcmp(GuAssets[a].name, g_all_assets[i].registry_name)) {
                    already = TRUE;
                    break;
                }
            }
            if (already) continue;

            VOID *data = NULL;
            s = ReadAssetFile(root, dir, g_all_assets[i].file_name, g_all_assets[i].size, &data);
            if (!EFI_ERROR(s) && data && GuAssetCount < GU_MAX_ASSETS) {
                GuAssets[GuAssetCount].name = g_all_assets[i].registry_name;
                GuAssets[GuAssetCount].firmware.size = g_all_assets[i].size;
                GuAssets[GuAssetCount].firmware.data = data;
                GuAssetCount++;

                if (i == 0) gsp_loaded = TRUE;
                else if (i >= 1 && i <= 4) ada_loaded = TRUE;
                else if (i >= 5 && i <= 8) ampere_loaded = TRUE;
            }
        }
    }

    root->Close(root);

    LogWrite("[Assets] Staged %u assets into RAM (GSP=%u, Ada=%u, Ampere=%u)\n",
             (unsigned)GuAssetCount, gsp_loaded, ada_loaded, ampere_loaded);

    if (!gsp_loaded || GuAssetCount < 5) {
        LogWrite("[Assets] Error: Incomplete firmware assets on boot media!\n");
        return EFI_NOT_FOUND;
    }

    /* Verify cryptographic SHA-256 signatures of all loaded payloads */
    if (!GuAssetsValid()) {
        LogWrite("[Assets] Error: Asset SHA-256 cryptographic verification failed!\n");
        return EFI_CRC_ERROR;
    }

    LogWrite("[Assets] Cryptographic SHA-256 verified for all %u assets (Ada + Ampere ready)\n",
             (unsigned)GuAssetCount);
    return EFI_SUCCESS;
}
