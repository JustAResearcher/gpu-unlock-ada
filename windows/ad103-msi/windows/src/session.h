/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#ifndef GU_SESSION_HELPER
#define GU_SESSION_HELPER 0
#endif
#if GU_SESSION_HELPER && !GU_ENABLE_UNLOCK
#error Session helpers require an explicit profile-bound experimental build
#endif

/* gu-session-v1: fixed binary ASCII fields, no terminators or padding.
 * OptionalData: GUSSN001 + nonce[32] + compiled profile SHA256[64].
 * REQUEST.BIN:  GUARM001 + the same fields, consumed as GUSED001.
 * The nonce is SessionId.ToString("N"), lowercase hexadecimal.
 */
#define GU_SESSION_BYTES 104U
EFI_STATUS GuSessionOpenParent(EFI_LOADED_IMAGE_PROTOCOL *Loaded,
  EFI_DEVICE_PATH_PROTOCOL *VolumePath, EFI_FILE_PROTOCOL *Root,
  EFI_FILE_PROTOCOL **Parent);
EFI_STATUS GuSessionConsume(EFI_FILE_PROTOCOL *Parent);
