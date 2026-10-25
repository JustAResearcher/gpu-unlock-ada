/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_VBIOS_610_H
#define GPU_UNLOCK_VBIOS_610_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

/* Production FWSEC descriptor extracted from a VBIOS image. */
struct gpu_unlock_fwsec {
	u32 image_off;
	u32 image_size;
	u32 imem_pa;
	u32 imem_size;
	u32 imem_va;
	u32 dmem_pa;
	u32 data_off;
	u32 dmem_size;
	u32 pkc_off;
	u32 interface_off;
	u32 ucode_id;
	u32 engine_mask;
	u32 sig_count;
	u32 sig_versions;
	u32 sig_off;
	u32 sig_bytes;
};

int gpu_unlock_vbios_fwsec(const u8 *rom, size_t length, struct gpu_unlock_fwsec *out);
int gpu_unlock_fwsec_patch_frts(u8 *image, const struct gpu_unlock_fwsec *fw,
				u64 frts_offset);
int gpu_unlock_fwsec_sig_offset(u32 versions, u32 fuse_version, u32 sig_size,
				u32 *offset);

#endif
