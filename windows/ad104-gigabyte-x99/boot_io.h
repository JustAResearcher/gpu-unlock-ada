/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuBootOpen(EFI_HANDLE,EFI_SYSTEM_TABLE *);
EFI_STATUS GuBootLoadAssets(VOID);
EFI_STATUS GuBootLoadWindows(VOID);
EFI_STATUS GuBootChoose(BOOLEAN *);
EFI_STATUS GuBootWindows(VOID);
EFI_STATUS GuBootClose(VOID);
VOID GuBootSay(const CHAR16 *);
VOID GuBootSilence(VOID);
VOID GuBootHalt(VOID);
/* v0.5.4: diagnostics log only while the USB log is open (GuLog faults otherwise). */
BOOLEAN GuBootLogOpen(VOID);
