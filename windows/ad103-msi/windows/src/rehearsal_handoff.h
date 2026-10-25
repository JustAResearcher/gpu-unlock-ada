/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
/* Same-boot observation and pinning, not authentication of the Windows image.
 * Prepare must precede the release adapter's baseline. Start never reselects.
 * Any StartImage return after driver mutation must be contained by the caller. */
EFI_STATUS GuRehearsalWindowsPrepare(EFI_SYSTEM_TABLE *Table);
EFI_STATUS GuRehearsalWindowsCancel(VOID);
EFI_STATUS GuRehearsalWindowsStart(VOID);
