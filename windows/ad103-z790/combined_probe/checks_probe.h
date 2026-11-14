/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port_probe.h"

typedef struct {
  UINT16 pm, pcie, msi, msix;
  UINT8 pcie_version;
} GU_CAPS;

/* Adapted from windows/ad104-msi/checks.c's GuCheckConfig: identical capability
 * walk and identity/cold-state checks, EXCEPT the PCIe bus number is not
 * asserted to equal any particular value -- that is genuinely board/slot
 * specific and this is the first run on this board, so it is only recorded
 * via the caller's own GetLocation() call, never gated here. */
BOOLEAN GuCheckConfigProbe(const UINT32 Config[64], UINTN Segment, UINTN Device,
                           UINTN Function, GU_CAPS *Out);
