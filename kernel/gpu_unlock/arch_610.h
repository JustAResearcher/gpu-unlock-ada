/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_ARCH_610_H
#define GPU_UNLOCK_ARCH_610_H

enum gpu_unlock_booter_arch {
	GU_BOOTER_GA10X = 0,
	GU_BOOTER_AD10X = 1,
};

/* Exact experimental AD103 canary; other Ada PCI IDs are not admitted. */
#define GU_AD103_RTX_4070_TI_SUPER 0x2705u
/* Exact experimental AD106 targets. 0x2805 has one exact-card canary;
 * 0x2803 has one exact-card canary with a matched stock/unlocked benchmark
 * (docs/ada-2803-report.md). */
#define GU_AD106_RTX_4060_TI_16GB 0x2805u
#define GU_AD106_RTX_4060_TI_16GB_2803 0x2803u
#define GU_IS_AD106_EXPERIMENTAL(dev)			\
	((dev) == GU_AD106_RTX_4060_TI_16GB ||		\
	 (dev) == GU_AD106_RTX_4060_TI_16GB_2803)

/* RTX 4090 D uses the AD102 firmware path; gated as experimental. */
#define GU_AD102_RTX_4090_D 0x2685u

#endif
