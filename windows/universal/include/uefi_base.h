/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIVERSAL_UEFI_BASE_H
#define UNIVERSAL_UEFI_BASE_H

#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef UINT8  u8;
typedef UINT16 u16;
typedef UINT32 u32;
typedef UINT64 u64;
typedef INT32  s32;
typedef INT64  s64;

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

/* Global UEFI services and image handle */
extern EFI_SYSTEM_TABLE        *gST;
extern EFI_BOOT_SERVICES        *gBS;
extern EFI_RUNTIME_SERVICES     *gRT;
extern EFI_HANDLE               gImageHandle;

/* Logging helpers */
VOID LogPrint(CONST CHAR16 *fmt, ...);
VOID LogStatus(CONST CHAR16 *prefix, EFI_STATUS status);

#endif /* UNIVERSAL_UEFI_BASE_H */

