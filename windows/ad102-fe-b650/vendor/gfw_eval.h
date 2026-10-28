/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_GFW_EVAL_H
#define GPU_UNLOCK_GFW_EVAL_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint16_t u16;
typedef uint32_t u32;
#endif

/*
 * Offsets and fields from the pinned 610.43.03 GA102 headers:
 * dev_gc6_island.h, dev_gc6_island_addendum.h, dev_gsp.h, dev_falcon_v4.h.
 * GFW completion uses kflcnWaitForHalt_TU102 (Falcon CPUCTL), not RISC-V CPUCTL.
 */
#define GU_GFW_PLM_OFFSET 0x00118128u
#define GU_GFW_BOOT_OFFSET 0x00118234u
#define GU_GFW_CPUCTL_OFFSET 0x00110100u
#define GU_GFW_TIMEOUT_US 2050000u
#define GU_FEAT_PLM_OFFSET 0x00823804u
#define GU_FEAT_SS0_OFFSET 0x0082381cu
#define GU_FEAT_SS1_OFFSET 0x00823820u
/* Nine 3-bit per-unit SM speed-select values, 0 = full rate (register report). */
#define GU_FEAT_READOUT_1_OFFSET 0x00823818u
#define GU_GFW_PLM_LEVEL0 0x1u
#define GU_GFW_BOOT_COMPLETED 0xffu
#define GU_GFW_CPUCTL_HALTED 0x10u
#define GU_PCI_COMMAND_IO 0x1u
#define GU_PCI_COMMAND_MEMORY 0x2u
#define GU_PCI_COMMAND_MASTER 0x4u

struct gpu_unlock_gfw_sample {
	bool config_ok;
	u32 plm;
	u32 boot;
	u32 cpuctl;
};

/* 0 complete, -EAGAIN still running, -ENODEV inaccessible. */
int gpu_unlock_gfw_assess(const struct gpu_unlock_gfw_sample *sample);

/* Put memory and bus-master bits back. Leave every other command bit alone. */
u16 gpu_unlock_command_restore(u16 original, u16 live);

#endif
