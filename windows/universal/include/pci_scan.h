/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIVERSAL_PCI_SCAN_H
#define UNIVERSAL_PCI_SCAN_H

#include "gpu_types.h"

/**
 * Scan all PCIe devices in the system, identify NVIDIA GPUs (RTX 30 and 40 series),
 * read BAR0 dynamically, inspect Subsystem IDs, and determine GOP display bindings.
 *
 * @param out_gpus Array to receive detected GPU records.
 * @param max_gpus Capacity of out_gpus array.
 * @return Number of detected supported NVIDIA GPUs.
 */
UINTN ScanSystemGpus(DETECTED_GPU out_gpus[], UINTN max_gpus);

/**
 * Formats strict ROM file path according to hardware ID:
 * \EFI\VBIOS\<vendor>_<device>_<subvendor>_<subdevice>.rom
 * e.g., "\EFI\VBIOS\10de_2782_1458_40e8.rom"
 */
VOID FormatGpuRomFileName(CONST DETECTED_GPU *gpu, CHAR16 *out_path, UINTN max_chars);

#endif /* UNIVERSAL_PCI_SCAN_H */

