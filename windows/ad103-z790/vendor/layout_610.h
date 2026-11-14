/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_LAYOUT_610_H
#define GPU_UNLOCK_LAYOUT_610_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#ifndef GPU_UNLOCK_U32_DEFINED
#define GPU_UNLOCK_U32_DEFINED
typedef uint32_t u32;
typedef uint64_t u64;
#endif
#endif

struct gpu_unlock_layout_in {
	u64 usable_fb;
	u64 vga_base;
	bool mmu_lock_valid;
	u64 mmu_lock_low;
	u64 frts_size;
	u64 boot_size;
	u64 image_size;
	u64 nonwpr_size;
	u64 heap_request;
};

struct gpu_unlock_layout_out {
	u64 reserved_top;
	u64 wpr_end;
	u64 frts_offset;
	u64 boot_offset;
	u64 gsp_offset;
	u64 heap_offset;
	u64 heap_size;
	u64 wpr_start;
	u64 nonwpr_start;
};

int gpu_unlock_layout(const struct gpu_unlock_layout_in *in,
		      struct gpu_unlock_layout_out *out);

#endif
