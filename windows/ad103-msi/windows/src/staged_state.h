/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"

/* Independently sample the completed unlock before/after firmware callbacks.
 * The caller must first validate the current PCI_IO interface and its ownership
 * graph. Both the transaction's exclusive claim and the restored firmware's
 * claim are supported; this check does not acquire or release either one.
 * No writes, allocations, console calls, or driver-binding calls are made.
 * Log=FALSE performs no file/log service calls and is valid after USB close.
 * A transport error/warning latches Gu.fault. No hardware state is repaired.
 */
EFI_STATUS GuStagedPostState(BOOLEAN Log);
