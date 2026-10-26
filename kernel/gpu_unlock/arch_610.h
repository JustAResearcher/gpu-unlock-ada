/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_ARCH_610_H
#define GPU_UNLOCK_ARCH_610_H

enum gpu_unlock_booter_arch {
	GU_BOOTER_GA10X = 0,
	GU_BOOTER_AD10X = 1,
};

/* Experimental Ada IDs. Each is admitted only by its own opt-in in main.c,
 * and all use the pinned AD102-family firmware (unlock_610.c). */
#define GU_AD102_RTX_4090_D 0x2685u
#define GU_AD102_RTX_4070_TI_SUPER 0x2689u /* cut-down AD102, not the AD103 0x2705 */
#define GU_AD103_RTX_4080_SUPER 0x2702u
#define GU_AD103_RTX_4080 0x2704u
#define GU_AD103_RTX_4070_TI_SUPER 0x2705u
#define GU_AD103_RTX_4070 0x2709u
#define GU_AD103M_RTX_4090_MOBILE 0x2757u
#define GU_AD104_RTX_4070_TI 0x2782u
#define GU_AD104_RTX_4070_SUPER 0x2783u
#define GU_AD104_RTX_4070 0x2786u
#define GU_AD106_RTX_4060_TI 0x2803u
#define GU_AD106_RTX_4060_TI_16GB 0x2805u
#define GU_AD107_RTX_4060 0x2882u
#define GU_AD107_RTX_4060_LAPTOP 0x28a0u
#define GU_AD107_RTX_4060_MOBILE 0x28e0u
#define GU_AD107_RTX_4050_MOBILE 0x28e1u

#endif
