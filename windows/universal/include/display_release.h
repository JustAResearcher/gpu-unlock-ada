/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIVERSAL_DISPLAY_RELEASE_H
#define UNIVERSAL_DISPLAY_RELEASE_H

#include "gpu_types.h"

/**
 * Universal Motherboard-Agnostic Display Release:
 * If the card has GOP bound (is currently displaying text/UEFI console),
 * disconnects the GOP driver using standard UEFI DisconnectController()
 * without hardcoded ACPI or chipset paths.
 *
 * If the card is Headless (compute only, not holding the display console),
 * display release is safely skipped, allowing the primary display card
 * to keep rendering text.
 */
EFI_STATUS ReleaseGpuDisplay(DETECTED_GPU *gpu);

/**
 * Restores PCI Command Decode (Memory Space + Bus Master) after controller disconnect.
 */
EFI_STATUS RestoreGpuPciDecode(DETECTED_GPU *gpu);

#endif /* UNIVERSAL_DISPLAY_RELEASE_H */

