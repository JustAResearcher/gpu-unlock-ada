// SPDX-License-Identifier: GPL-2.0-only
#include "diagnostic.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#endif

#include "prep_610.h"

int gpu_unlock_booter_validate(enum gpu_unlock_booter_arch arch,
			       const struct gpu_unlock_booter_hdr *hdr,
			       u32 image_size, u32 sig_bytes)
{
	u32 expected_image, data_off, data_size, app_size, patch_loc;

	if (!hdr)
		return GU_DIAG(-EINVAL);
	switch (arch) {
	case GU_BOOTER_GA10X:
		expected_image = 60416;
		data_off = 35328;
		data_size = 25088;
		app_size = 35072;
		patch_loc = 35344;
		break;
	case GU_BOOTER_AD10X:
		/* NVIDIA 610.43.03 AD102 archive, also selected on AD103..AD107.
		 * Validating stock geometry does not validate a V67 payload. */
		expected_image = 56832;
		data_off = 32256;
		data_size = 24576;
		app_size = 32000;
		patch_loc = 32272;
		break;
	default:
		return GU_DIAG(-EOPNOTSUPP);
	}
	if (image_size != expected_image || sig_bytes != GU_BOOTER_SIG_TOTAL ||
	    hdr->sig_bytes != sig_bytes || hdr->ucode_id != 3 ||
	    hdr->engine_id != 1 || hdr->num_sigs != 2 || hdr->num_apps != 1 ||
	    hdr->os_code_off != 0 || hdr->os_code_size != 256 ||
	    hdr->os_data_off != data_off || hdr->os_data_size != data_size ||
	    hdr->app_code_off != 256 || hdr->app_code_size != app_size ||
	    hdr->app_data_off != 256 || hdr->app_data_size != 0 ||
	    hdr->patch_loc != patch_loc)
		return GU_DIAG(-EINVAL);
	return 0;
}
u32 gpu_unlock_fuse_version(u32 raw)
{
	u32 index = 0;
	u32 value = raw & 0xffffu;

	if (!value)
		return 0;
	while (value > 1) {
		value >>= 1;
		index++;
	}
	return index + 1;
}

static const struct gpu_unlock_booter_geometry gpu_unlock_boot_fws[] = {
	{	/* GA102 BooterLoadUcode, 610.43.03 */
		.image_size = GU_BOOTER_IMAGE_SIZE,
		.sig_bytes = GU_BOOTER_SIG_TOTAL,
		.hdr = {
			.os_code_off = 0, .os_code_size = 256,
			.os_data_off = 35328, .os_data_size = 25088,
			.num_apps = 1, .app_code_off = 256, .app_code_size = 35072,
			.app_data_off = 256, .app_data_size = 0,
			.patch_loc = 35344, .ucode_id = 3, .engine_id = 1,
			.num_sigs = 2, .sig_bytes = GU_BOOTER_SIG_TOTAL,
		},
	},
	{	/* AD102 BooterLoadUcode, 610.43.03 */
		.image_size = GU_BOOTER_IMAGE_SIZE_AD102,
		.sig_bytes = GU_BOOTER_SIG_TOTAL,
		.hdr = {
			.os_code_off = 0, .os_code_size = 256,
			.os_data_off = 32256, .os_data_size = 24576,
			.num_apps = 1, .app_code_off = 256, .app_code_size = 32000,
			.app_data_off = 256, .app_data_size = 0,
			.patch_loc = 32272, .ucode_id = 3, .engine_id = 1,
			.num_sigs = 2, .sig_bytes = GU_BOOTER_SIG_TOTAL,
		},
	},
};

const struct gpu_unlock_booter_geometry *
gpu_unlock_booter_geometry_match(const struct gpu_unlock_booter_hdr *hdr,
				 u32 image_size, u32 sig_bytes)
{
	unsigned int i;

	if (!hdr)
		return NULL;
	for (i = 0; i < sizeof(gpu_unlock_boot_fws) / sizeof(gpu_unlock_boot_fws[0]); i++) {
		const struct gpu_unlock_booter_geometry *fw = &gpu_unlock_boot_fws[i];

		if (fw->image_size == image_size && fw->sig_bytes == sig_bytes &&
		    hdr->sig_bytes == sig_bytes &&
		    !memcmp(&fw->hdr, hdr, sizeof(*hdr)))
			return fw;
	}
	return NULL;
}

int gpu_unlock_booter_patch(u8 *image, u32 image_size,
			    const struct gpu_unlock_booter_hdr *hdr,
			    const u8 *signatures, u32 sig_bytes, u32 fuse_ver)
{
	u32 index, sig_size;

	if (!image || !hdr || !signatures)
		return GU_DIAG(-EINVAL);
	if (hdr->num_apps != 1 || !hdr->num_sigs)
		return GU_DIAG(-EINVAL);
	if (sig_bytes != hdr->sig_bytes || sig_bytes % hdr->num_sigs)
		return GU_DIAG(-EINVAL);
	sig_size = sig_bytes / hdr->num_sigs;
	if (!sig_size || image_size < hdr->patch_loc ||
	    image_size - hdr->patch_loc < sig_size)
		return GU_DIAG(-EINVAL);
	if (hdr->num_sigs > 1) {
		if (fuse_ver >= hdr->num_sigs)
			return GU_DIAG(-ERANGE);
		index = hdr->num_sigs - 1 - fuse_ver;
	} else {
		index = 0;
	}
	memcpy(image + hdr->patch_loc, signatures + (size_t)index * sig_size,
	       sig_size);
	return 0;
}

static u16 rd16(const u8 *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

static u32 rd32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 rd64(const u8 *p)
{
	return (u64)rd32(p) | ((u64)rd32(p + 4) << 32);
}

int gpu_unlock_elf_section(const u8 *elf, size_t length, const char *name,
			   struct gpu_unlock_span *span)
{
	u64 shoff;
	u16 shentsize, shnum, shstrndx, i;
	u32 name_off;
	const u8 *shstr;
	u64 shstr_off, shstr_size;
	bool found = false;

	if (!elf || !name || !span || length < 64)
		return GU_DIAG(-EINVAL);
	if (elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F')
		return GU_DIAG(-EINVAL);
	if (elf[4] != 2 || elf[5] != 1 || elf[6] != 1 ||
	    rd32(elf + 20) != 1 || rd16(elf + 52) != 64)
		return GU_DIAG(-EINVAL);
	shoff = rd64(elf + 40);
	shentsize = rd16(elf + 58);
	shnum = rd16(elf + 60);
	shstrndx = rd16(elf + 62);
	if (shentsize != 64 || !shnum || shstrndx >= shnum)
		return GU_DIAG(-EINVAL);
	if (shoff > length || (u64)shnum * shentsize > length - shoff)
		return GU_DIAG(-EINVAL);
	shstr_off = rd64(elf + shoff + (u64)shstrndx * shentsize + 24);
	shstr_size = rd64(elf + shoff + (u64)shstrndx * shentsize + 32);
	if (shstr_off > length || shstr_size > length - shstr_off)
		return GU_DIAG(-EINVAL);
	shstr = elf + shstr_off;
	for (i = 0; i < shnum; i++) {
		const u8 *sh = elf + shoff + (u64)i * shentsize;
		u64 off = rd64(sh + 24);
		u64 size = rd64(sh + 32);
		size_t namelen = strlen(name);

		name_off = rd32(sh);
		if ((u64)name_off >= shstr_size)
			return GU_DIAG(-EINVAL);
		if (!memchr(shstr + name_off, 0, shstr_size - name_off))
			return GU_DIAG(-EINVAL);
		if (namelen >= shstr_size - name_off ||
		    memcmp(shstr + name_off, name, namelen) ||
		    shstr[name_off + namelen] != '\0')
			continue;
		if (found || off > length || size > length - off)
			return GU_DIAG(-EINVAL);
		span->offset = off;
		span->size = size;
		found = true;
	}
	return GU_DIAG(found ? 0 : -ENOENT);
}

int gpu_unlock_radix_plan(u64 size, struct gpu_unlock_radix_plan *plan)
{
	u64 pages[4];
	unsigned int level;
	u64 tables = 0;

	if (!plan || !size)
		return GU_DIAG(-EINVAL);
	if (size > ~(u64)0 - (GU_RADIX_PAGE - 1))
		return GU_DIAG(-EOVERFLOW);
	memset(plan, 0, sizeof(*plan));
	pages[3] = (size + GU_RADIX_PAGE - 1) >> 12;
	for (level = 3; level > 0; level--)
		pages[level - 1] = ((pages[level] - 1) >> 9) + 1;
	if (pages[0] != 1)
		return GU_DIAG(-ERANGE);
	for (level = 0; level < 3; level++)
		tables += pages[level];
	plan->pages[0] = pages[0];
	plan->pages[1] = pages[1];
	plan->pages[2] = pages[2];
	plan->pages[3] = pages[3];
	plan->table_pages = tables;
	plan->table_bytes = tables << 12;
	plan->data_bytes = pages[3] << 12;
	return 0;
}

static void wr64(u8 *p, u64 value)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		p[i] = (u8)(value >> (8 * i));
}

int gpu_unlock_boot_desc_parse(const u8 *data, size_t size, u32 image_size,
			       struct gpu_unlock_boot_desc *out)
{
	static const unsigned int ranges[] = { 1, 3, 8, 10, 12 };
	unsigned int i;

	if (!data || !out || size != 84 || !image_size ||
	    rd32(data) != 5 || rd32(data + 56) != 1 ||
	    rd32(data + 76) != image_size || rd32(data + 80))
		return GU_DIAG(-EINVAL);
	for (i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		u32 off = rd32(data + ranges[i] * 4);
		u32 len = rd32(data + (ranges[i] + 1) * 4);

		if (!len || off > image_size || len > image_size - off)
			return GU_DIAG(-ERANGE);
	}
	/* This path doesn't boot a secure partition, load ITCM or enable SMP. */
	for (i = 5; i <= 7; i++)
		if (rd32(data + i * 4))
			return GU_DIAG(-EOPNOTSUPP);
	for (i = 15; i <= 18; i++)
		if (rd32(data + i * 4))
			return GU_DIAG(-EOPNOTSUPP);
	out->manifest_offset = rd32(data + 32);
	out->data_offset = rd32(data + 40);
	out->code_offset = rd32(data + 48);
	out->image_size = image_size;
	return 0;
}

int gpu_unlock_radix_page(u8 out[GU_RADIX_PAGE], const u64 *children, size_t count)
{
	size_t i;

	if (!out || !children || !count || count > GU_RADIX_PAGE / 8)
		return GU_DIAG(-EINVAL);
	for (i = 0; i < count; i++)
		if (children[i] & (GU_RADIX_PAGE - 1))
			return GU_DIAG(-EINVAL);
	memset(out, 0, GU_RADIX_PAGE);
	for (i = 0; i < count; i++)
		wr64(out + i * 8, children[i]);
	return 0;
}

int gpu_unlock_wpr_meta_pack(u8 out[GU_WPR_META_SIZE],
			     const struct gpu_unlock_layout_out *layout,
			     const struct gpu_unlock_wpr_args *a)
{
	if (!out || !layout || !a || !a->signature_size || !a->image_size ||
	    !a->boot.image_size || !a->fb_size || !a->frts_size ||
	    a->vga_offset >= a->fb_size ||
	    a->boot.code_offset >= a->boot.image_size ||
	    a->boot.data_offset >= a->boot.image_size ||
	    a->boot.manifest_offset >= a->boot.image_size ||
	    (a->radix_dma & 4095) || (a->boot_dma & 4095) ||
	    layout->nonwpr_start >= layout->wpr_start ||
	    layout->wpr_start >= layout->heap_offset ||
	    layout->heap_offset >= layout->gsp_offset ||
	    layout->heap_size > layout->gsp_offset - layout->heap_offset ||
	    layout->gsp_offset >= layout->boot_offset ||
	    a->image_size > layout->boot_offset - layout->gsp_offset ||
	    layout->boot_offset >= layout->frts_offset ||
	    a->boot.image_size > layout->frts_offset - layout->boot_offset ||
	    layout->frts_offset >= layout->wpr_end ||
	    a->frts_size != layout->wpr_end - layout->frts_offset ||
	    layout->wpr_end > a->vga_offset)
		return GU_DIAG(-EINVAL);
	memset(out, 0, GU_WPR_META_SIZE);
	wr64(out + 0, GU_WPR_META_MAGIC);
	wr64(out + 8, 1);
	wr64(out + 16, a->radix_dma);
	wr64(out + 24, a->image_size);
	wr64(out + 32, a->boot_dma);
	wr64(out + 40, a->boot.image_size);
	wr64(out + 48, a->boot.code_offset);
	wr64(out + 56, a->boot.data_offset);
	wr64(out + 64, a->boot.manifest_offset);
	wr64(out + 72, a->signature_dma);
	wr64(out + 80, a->signature_size);
	wr64(out + 88, layout->nonwpr_start);
	wr64(out + 96, layout->nonwpr_start);
	wr64(out + 104, layout->wpr_start - layout->nonwpr_start);
	wr64(out + 112, layout->wpr_start);
	wr64(out + 120, layout->heap_offset);
	wr64(out + 128, layout->heap_size);
	wr64(out + 136, layout->gsp_offset);
	wr64(out + 144, layout->boot_offset);
	wr64(out + 152, layout->frts_offset);
	wr64(out + 160, a->frts_size);
	wr64(out + 168, layout->wpr_end);
	wr64(out + 176, a->fb_size);
	wr64(out + 184, a->vga_offset);
	wr64(out + 192, a->fb_size - a->vga_offset);
	return 0;
}

u32 gpu_unlock_dma_cmd(bool imem)
{
	u32 cmd = 6u << 8;

	if (imem)
		cmd |= (1u << 4) | (1u << 2);
	return cmd;
}
