/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
/* One-shot firmware-driver rehearsal. The caller performs board/ROM checks,
 * preloads Windows, and obtains explicit user arming before Run. A failure
 * after Mutated() becomes true must halt, never return or start Windows.
 * Restored() is a fresh, read-only verification suitable before handoff.
 * No unlock, reset, DMA, PCI writes, NVRAM writes, or automatic rollback. */
EFI_STATUS GuRehearsalPrepare(EFI_SYSTEM_TABLE *Table);
EFI_STATUS GuRehearsalRun(VOID);
EFI_STATUS GuRehearsalRestored(VOID);
BOOLEAN GuRehearsalMutated(VOID);
/* The separate staged-unlock build inserts one exclusively owned transaction
 * between the proven release and restore sequences. These entry points deny
 * access in a rehearsal build. Any failure after release requires a halt. */
EFI_STATUS GuStagedRelease(VOID);
EFI_STATUS GuStagedClaim(VOID);
BOOLEAN GuStagedDetached(VOID);
EFI_STATUS GuStagedComplete(VOID);
EFI_STATUS GuStagedRestore(VOID);
/* Halt-only containment refresh. The caller masks notifications at TPL_NOTIFY.
 * No logging, writes, graph assumptions, or changes to transaction/fault flags.
 * Gu.pci remains NULL unless the original pinned PCI interface is revalidated. */
EFI_STATUS GuStagedRefreshPci(VOID);
