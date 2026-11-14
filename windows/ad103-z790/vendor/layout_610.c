// SPDX-License-Identifier: GPL-2.0-only
#include "diagnostic.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#include "layout_610.h"

static int align_down(u64 value, u64 align, u64 *out)
{
	if (!align || (align & (align - 1)) || value < align)
		return GU_DIAG(-EINVAL);
	*out = value & ~(align - 1);
	return 0;
}

static int align_up(u64 value, u64 align, u64 *out)
{
	u64 down;

	if (!align || (align & (align - 1)))
		return GU_DIAG(-EINVAL);
	if (value > ~(u64)0 - (align - 1))
		return GU_DIAG(-EOVERFLOW);
	if (align_down(value + align - 1, align, &down))
		return GU_DIAG(-EINVAL);
	*out = down;
	return 0;
}

static int sub_u64(u64 value, u64 amount, u64 *out)
{
	if (value < amount)
		return GU_DIAG(-ERANGE);
	*out = value - amount;
	return 0;
}

int gpu_unlock_layout(const struct gpu_unlock_layout_in *in,
		      struct gpu_unlock_layout_out *out)
{
	u64 reserved, wpr_end, frts, boot, gsp, nonwpr, meta, heap;
	u64 heap_off, wpr_start, nonwpr_start;
	int ret;

	if (!in || !out)
		return GU_DIAG(-EINVAL);
	if (!in->usable_fb || !in->vga_base || !in->frts_size || !in->boot_size ||
	    !in->image_size)
		return GU_DIAG(-EINVAL);
	reserved = in->vga_base;
	if (in->mmu_lock_valid && in->mmu_lock_low < reserved)
		reserved = in->mmu_lock_low;
	ret = align_down(reserved, 128ull * 1024, &wpr_end);
	if (ret)
		return GU_DIAG(ret);
	ret = sub_u64(wpr_end, in->frts_size, &frts);
	if (ret)
		return GU_DIAG(ret);
	ret = sub_u64(frts, in->boot_size, &boot);
	if (ret)
		return GU_DIAG(ret);
	ret = align_down(boot, 4096, &boot);
	if (ret)
		return GU_DIAG(ret);
	ret = sub_u64(boot, in->image_size, &gsp);
	if (ret)
		return GU_DIAG(ret);
	ret = align_down(gsp, 64ull * 1024, &gsp);
	if (ret)
		return GU_DIAG(ret);
	ret = align_up(in->nonwpr_size, 1024ull * 1024, &nonwpr);
	if (ret)
		return GU_DIAG(ret);
	ret = align_up(256, 1024ull * 1024, &meta);
	if (ret)
		return GU_DIAG(ret);
	ret = align_up(in->heap_request, 1024ull * 1024, &heap);
	if (ret)
		return GU_DIAG(ret);
	ret = sub_u64(gsp, heap, &heap_off);
	if (ret)
		return GU_DIAG(ret);
	ret = align_down(heap_off, 1024ull * 1024, &heap_off);
	if (ret)
		return GU_DIAG(ret);
	ret = sub_u64(heap_off, meta, &wpr_start);
	if (ret)
		return GU_DIAG(ret);
	ret = sub_u64(wpr_start, nonwpr, &nonwpr_start);
	if (ret)
		return GU_DIAG(ret);
	if (nonwpr_start >= in->usable_fb || wpr_end > in->usable_fb ||
	    wpr_start >= wpr_end || gsp >= boot || boot >= frts || frts >= wpr_end)
		return GU_DIAG(-ERANGE);
	out->reserved_top = reserved;
	out->wpr_end = wpr_end;
	out->frts_offset = frts;
	out->boot_offset = boot;
	out->gsp_offset = gsp;
	out->heap_offset = heap_off;
	out->heap_size = (gsp - heap_off) & ~((1ull << 20) - 1);
	out->wpr_start = wpr_start;
	out->nonwpr_start = nonwpr_start;
	return 0;
}
