/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
EFI_STATUS GuProbeProm(BOOLEAN Verify);
VOID GuProfileLogLocation(UINTN Segment, UINTN Bus, UINTN Device, UINTN Function,
                          const UINT32 Config[64]);
