/* SPDX-License-Identifier: MIT */
#pragma once
#include "core.h"
typedef struct {UINT32 rva,bytes,relocations;} CODE_VIEW;
BOOLEAN LoadedCodeView(const UINT8 *,UINTN,CODE_VIEW *);
void SamplePlatform(TEXT *,EFI_SYSTEM_TABLE *,EFI_HANDLE);
