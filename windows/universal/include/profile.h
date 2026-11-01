/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
#include "vendor/vbios_610.h"
BOOLEAN GuAssetsValid(VOID);
BOOLEAN GuAd104AssetsValid(VOID);
BOOLEAN GuAd104PromMatches(const VOID *, UINTN);
BOOLEAN GuAd104FwsecMatches(const struct gpu_unlock_fwsec *);
/* Prepare is read-only on the GPU. It validates ROM, assets, exact PCI/card
 * state and creates the immutable selector latch. No driver release here. */
EFI_STATUS GuAd104Prepare(VOID);
BOOLEAN GuAd104ColdEntry(VOID);
