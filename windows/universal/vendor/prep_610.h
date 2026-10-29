/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_PREP_610_H
#define GPU_UNLOCK_PREP_610_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
#define GPU_UNLOCK_U32_DEFINED
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#include "layout_610.h"
#include "arch_610.h"

#define GU_BOOTER_IMAGE_SIZE 60416u
#define GU_BOOTER_IMAGE_SIZE_AD102 56832u
#define GU_BOOTER_SIG_TOTAL 768u

/* Tested AD102 PCI ID using the pinned AD102-family Booter assets. */
#define GU_AD102_RTX_4090 0x2684u
#define GU_V67_SIZE 0xfa00u
#define GU_WPR_META_SIZE 256u
#define GU_WPR_META_MAGIC 0xdc3aae21371a60b3ull
#define GU_RADIX_PAGE 4096u
#define GU_FUSE_SEC2_UCODE1 0x00824140u

struct gpu_unlock_booter_hdr {
	u32 os_code_off;
	u32 os_code_size;
	u32 os_data_off;
	u32 os_data_size;
	u32 num_apps;
	u32 app_code_off;
	u32 app_code_size;
	u32 app_data_off;
	u32 app_data_size;
	u32 patch_loc;
	u32 ucode_id;
	u32 engine_id;
	u32 num_sigs;
	u32 sig_bytes;
};

int gpu_unlock_booter_validate(enum gpu_unlock_booter_arch arch,
			       const struct gpu_unlock_booter_hdr *hdr,
			       u32 image_size, u32 sig_bytes);

/* One pinned BooterLoad container geometry. Only these are accepted, so a
 * wrong or truncated asset cannot reach the Falcon. */
struct gpu_unlock_booter_geometry {
	u32 image_size;
	u32 sig_bytes;
	struct gpu_unlock_booter_hdr hdr;
};

struct gpu_unlock_span {
	u64 offset;
	u64 size;
};

struct gpu_unlock_radix_plan {
	u64 pages[4];
	u64 table_pages;
	u64 table_bytes;
	u64 data_bytes;
};

/* Version-5 production boot descriptor, decoded from the pinned wire format. */
struct gpu_unlock_boot_desc {
	u32 code_offset;
	u32 data_offset;
	u32 manifest_offset;
	u32 image_size;
};

struct gpu_unlock_wpr_args {
	u64 fb_size, vga_offset, frts_size;
	u64 signature_dma, signature_size;
	u64 radix_dma, image_size;
	u64 boot_dma;
	struct gpu_unlock_boot_desc boot;
};

int gpu_unlock_boot_desc_parse(const u8 *data, size_t size, u32 image_size,
			       struct gpu_unlock_boot_desc *out);

/* One table page; children are device DMA addresses, never CPU pointers. */
int gpu_unlock_radix_page(u8 out[GU_RADIX_PAGE], const u64 *children, size_t count);

/* SEC2 fuse register value to the Booter signature index input. */
u32 gpu_unlock_fuse_version(u32 raw);

/* The pinned geometry matching this header and container size, or NULL. */
const struct gpu_unlock_booter_geometry *
gpu_unlock_booter_geometry_match(const struct gpu_unlock_booter_hdr *hdr,
				 u32 image_size, u32 sig_bytes);

int gpu_unlock_booter_patch(u8 *image, u32 image_size,
			    const struct gpu_unlock_booter_hdr *hdr,
			    const u8 *signatures, u32 sig_bytes, u32 fuse_ver);

int gpu_unlock_elf_section(const u8 *elf, size_t length, const char *name,
			   struct gpu_unlock_span *span);

int gpu_unlock_radix_plan(u64 size, struct gpu_unlock_radix_plan *plan);

int gpu_unlock_wpr_meta_pack(u8 out[GU_WPR_META_SIZE],
			     const struct gpu_unlock_layout_out *layout,
			     const struct gpu_unlock_wpr_args *args);

u32 gpu_unlock_dma_cmd(bool imem);

#endif
