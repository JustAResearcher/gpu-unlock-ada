/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIVERSAL_VBIOS_LOADER_H
#define UNIVERSAL_VBIOS_LOADER_H

#include "gpu_types.h"

/**
 * Searches the boot volume (FAT32 partition) for a VBIOS file matching
 * STRICTLY the hardware ID format:
 *   \EFI\VBIOS\<vendor>_<device>_<subvendor>_<subdevice>.rom
 * (Case-insensitive check: lowercase and uppercase).
 *
 * If another card in all_gpus has the identical hardware ID and its VBIOS
 * is already loaded in RAM, reuses the existing RAM buffer (no duplicate I/O).
 *
 * NOTE: Strict mode enforced. Generic filenames like 'vbios.rom' are rejected.
 *
 * @param target_gpu GPU record to populate with VBIOS buffer in RAM.
 * @param all_gpus System GPU array (to check for identical sibling cards).
 * @param gpu_count Total number of GPUs in all_gpus.
 * @return EFI_SUCCESS if found and read into RAM, EFI_NOT_FOUND otherwise.
 */
EFI_STATUS LoadGpuVbiosStrictId(DETECTED_GPU *target_gpu, CONST DETECTED_GPU all_gpus[], UINTN gpu_count);

/**
 * Frees VBIOS RAM buffer allocated by LoadGpuVbiosStrictId.
 */
VOID FreeGpuVbios(DETECTED_GPU *gpu);

#endif /* UNIVERSAL_VBIOS_LOADER_H */

