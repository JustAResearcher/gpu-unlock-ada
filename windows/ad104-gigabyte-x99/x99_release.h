/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"

EFI_STATUS GuX99Preflight(EFI_SYSTEM_TABLE *table, EFI_HANDLE self, EFI_HANDLE pci);
EFI_STATUS GuX99Release(VOID);
BOOLEAN GuX99Released(VOID);
EFI_STATUS GuX99Finish(VOID);
BOOLEAN GuX99ReadyForWindows(VOID);
BOOLEAN GuX99Mutated(VOID);

