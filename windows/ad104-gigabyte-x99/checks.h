/* Modified 2026-10-19/20 for Windows GPU UNLOCK; changes documented in CHANGES.md. */ #pragma once

#include "port.h"

typedef struct {
  UINT16 pm, pcie, msi, msix;
  UINT8 pcie_version;
} GU_CAPS;

/* Config is a complete, CPU-owned 256-byte snapshot. Out is cleared on failure. */
BOOLEAN GuCheckConfig(const UINT32 Config[64], UINTN Segment, UINTN Bus,
                      UINTN Device, UINTN Function, GU_CAPS *Out);

/* Path must point to readable memory through its terminator, up to MaxBytes.
   MaxBytes is a read bound, not necessarily the exact allocation length.
   Returns 1 for the exact captured AMD GOP route/ADR, 2 for RTX, 0 otherwise.
   Route alone does not establish PCI identity: validate the GOP PCI parent. */
int GuDisplayPath(const EFI_DEVICE_PATH_PROTOCOL *Path, UINTN MaxBytes);
