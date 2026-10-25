/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_PLATFORM_610_H
#define GPU_UNLOCK_PLATFORM_610_H
/* UEFI PCI protocol failures are fallible unlike Linux readl/writel.  Keep
 * DMA lifetime decisions in the shared transaction; the adapter mirrors them. */
#ifdef GU_UEFI_PORT
bool gpu_unlock_platform_fault(void);
void gpu_unlock_platform_started(void);
void gpu_unlock_platform_quiesced(void);
#else
static inline bool gpu_unlock_platform_fault(void) { return false; }
static inline void gpu_unlock_platform_started(void) { }
static inline void gpu_unlock_platform_quiesced(void) { }
#endif
#endif
