/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
#include "vendor/vbios_610.h"
BOOLEAN GuAd103AssetsValid(VOID);
BOOLEAN GuAd103PromMatches(const VOID *, UINTN);
BOOLEAN GuAd103FwsecMatches(const struct gpu_unlock_fwsec *);
/* Prepare is read-only on the GPU. It validates ROM, assets, exact PCI/card
 * state and creates the immutable selector latch. No driver release here. */
EFI_STATUS GuAd103Prepare(VOID);
BOOLEAN GuAd103ColdEntry(VOID);
