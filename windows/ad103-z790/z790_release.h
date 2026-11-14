/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuZ790Preflight(EFI_SYSTEM_TABLE *,EFI_HANDLE,EFI_HANDLE);
EFI_STATUS GuZ790Release(VOID);
BOOLEAN GuZ790Released(VOID);
EFI_STATUS GuZ790Finish(VOID);
BOOLEAN GuZ790ReadyForWindows(VOID);
BOOLEAN GuZ790Mutated(VOID);
