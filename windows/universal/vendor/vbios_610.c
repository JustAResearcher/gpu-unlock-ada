// SPDX-License-Identifier: GPL-2.0-only
#include "diagnostic.h"
/* Formats: NVIDIA 610.43.03 kernel_gsp_fwsec.c and kernel_gsp_vbios_tu102.c.
 * Independent byte parser; no firmware-owned pointer is trusted as a host pointer.
 */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif
#include "vbios_610.h"

static u16 r16(const u8 *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

static u32 r32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void w32(u8 *p, u32 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
	p[2] = (u8)(value >> 16);
	p[3] = (u8)(value >> 24);
}

static int range(size_t length, u64 off, u64 bytes)
{
	return off > length || bytes > length - off ? -EINVAL : 0;
}

/* kgspExtractVbiosFromRom_TU102 / s_locateExpansionRoms. Image signatures are
 * 0xAA55, 0x4E56 ("VN") and 0xBB77. Data signatures are PCIR, NPDS and RGIS.
 */
static int rom_signature(u16 sig)
{
	return sig == 0xaa55 || sig == 0x4e56 || sig == 0xbb77;
}

static int data_signature(u32 sig)
{
	return sig == 0x52494350 || sig == 0x5344504e || sig == 0x53494752;
}

static int expansion(const u8 *rom, size_t length, u32 *adjust)
{
	u32 pos = 0, base = 0, ext = 0;
	unsigned int images = 0;

	if (range(length, 0, 2) || !rom_signature(r16(rom)))
		return GU_DIAG(-EINVAL);
	for (;;) {
		u64 pcir, npde;
		u32 bytes, last, sig;
		u16 ext_rev, ext_len;

		if (++images > 8 || range(length, pos, 0x1a) || !rom_signature(r16(rom + pos)))
			return GU_DIAG(-EINVAL);
		pcir = (u64)pos + r16(rom + pos + 0x18);
		if (range(length, pcir, 0x16))
			return GU_DIAG(-EINVAL);
		sig = r32(rom + pcir);
		if (!data_signature(sig))
			return GU_DIAG(-EINVAL);
		bytes = (u32)r16(rom + pcir + 0x10) * 512;
		last = rom[pcir + 0x15] & 0x80;
		npde = (pcir + r16(rom + pcir + 0x0a) + 15) & ~(u64)15;
		if (!range(length, npde, 4) && r32(rom + npde) == 0x4544504e) {
			if (range(length, npde, 10))
				return GU_DIAG(-EINVAL);
			ext_rev = r16(rom + npde + 4);
			ext_len = r16(rom + npde + 6);
			if (ext_rev == 0x100 || ext_rev == 0x101) {
				u32 sub = (u32)r16(rom + npde + 8) * 512;

				if (ext_len < 10 || range(length, npde, ext_len))
					return GU_DIAG(-EINVAL);
				if (ext_len > 10)
					last = rom[npde + 10] & 0x80;
				else if (sub < bytes)
					last = 0;
				bytes = sub;
			}
		}
		GU_NOTE("ROM chain: pos=%#x data=%#x bytes=%u type=%#x last=%u\n",
			pos, sig, bytes, rom[pcir + 0x14], last);
		if (!bytes || range(length, pos, bytes))
			return GU_DIAG(-EINVAL);
		if (!base && rom[pcir + 0x14] == 0)
			base = bytes;
		if (!ext && rom[pcir + 0x14] == 0xe0)
			ext = pos;
		if (last)
			break;
		pos += bytes;
	}
	if (ext && base && ext < base)
		return GU_DIAG(-EINVAL);
	*adjust = ext && base ? ext - base : 0;
	GU_NOTE("ROM expansion: base=%#x ext=%#x adjust=%#x\n", base, ext, *adjust);
	return 0;
}

static int descriptor(const u8 *rom, size_t length, u64 off,
		      struct gpu_unlock_fwsec *out)
{
	struct gpu_unlock_fwsec fw = { 0 };
	u32 hdr, size, stored, bits, count = 0;

	if (range(length, off, 44))
		return GU_DIAG(-EINVAL);
	hdr = r32(rom + off);
	size = hdr >> 16;
	GU_NOTE("FWSEC descriptor: offset=%#llx ROM_bytes=%zu header=%#x\n",
		(unsigned long long)off, length, hdr);
	/* vDesc: bit 0 set, version in bits 15:8, size in bits 31:16. */
	if (!(hdr & 1) || ((hdr >> 8) & 0xff) != 3 || size < 44)
		return GU_DIAG(-EOPNOTSUPP);
	if (range(length, off, size))
		return GU_DIAG(-EINVAL);
	stored = r32(rom + off + 4);
	if (!stored || stored > ~(u32)0 - 255)
		return GU_DIAG(-EINVAL);
	fw.image_size = (stored + 255) & ~255u;
	if (range(length, off + size, fw.image_size))
		return GU_DIAG(-EINVAL);
	fw.image_off = off + size;
	fw.pkc_off = r32(rom + off + 8);
	fw.interface_off = r32(rom + off + 12);
	fw.imem_pa = r32(rom + off + 16);
	fw.imem_size = r32(rom + off + 20);
	fw.imem_va = r32(rom + off + 24);
	fw.dmem_pa = r32(rom + off + 28);
	fw.dmem_size = r32(rom + off + 32);
	fw.data_off = fw.imem_size;
	fw.engine_mask = r16(rom + off + 36);
	fw.ucode_id = rom[off + 38];
	fw.sig_count = rom[off + 39];
	fw.sig_versions = r16(rom + off + 40);
	fw.sig_off = off + 44;
	fw.sig_bytes = size - 44;
	GU_NOTE("FWSEC geometry: image_off=%#x size=%#x imem=%#x va=%#x pa=%#x dmem=%#x pa=%#x pkc=%#x interface=%#x engine=%#x ucode=%u signatures=%u versions=%#x bytes=%u\n",
		fw.image_off, fw.image_size, fw.imem_size, fw.imem_va, fw.imem_pa,
		fw.dmem_size, fw.dmem_pa, fw.pkc_off, fw.interface_off, fw.engine_mask,
		fw.ucode_id, fw.sig_count, fw.sig_versions, fw.sig_bytes);
	bits = fw.sig_versions;
	while (bits) {
		count += bits & 1;
		bits >>= 1;
	}
	if (!fw.ucode_id || fw.ucode_id > 16 || !fw.engine_mask || !count ||
	    count != fw.sig_count || fw.sig_bytes != count * 384 ||
	    !fw.imem_size || !fw.dmem_size ||
	    range(fw.image_size, fw.data_off, fw.dmem_size) ||
	    range(fw.dmem_size, fw.pkc_off, 384) ||
	    range(fw.dmem_size, fw.interface_off, 4))
		return GU_DIAG(-EINVAL);
	*out = fw;
	return 0;
}

int gpu_unlock_vbios_fwsec(const u8 *rom, size_t length, struct gpu_unlock_fwsec *out)
{
	u32 bit, adjust;
	int ret;

	if (!rom || !out || length > 1024 * 1024)
		return GU_DIAG(-EINVAL);
	memset(out, 0, sizeof(*out));
	ret = expansion(rom, length, &adjust);
	if (ret)
		return GU_DIAG(ret);
	for (bit = 0; bit + 12 <= length; bit++) {
		u32 size, stride, tokens, sum = 0, i;

		if (r16(rom + bit) != 0xb8ff || r32(rom + bit + 2) != 0x00544942)
			continue;
		size = rom[bit + 8];
		stride = rom[bit + 9];
		tokens = rom[bit + 10];
		if (size < 12 || stride < 6 || range(length, bit, size))
			return GU_DIAG(-EINVAL);
		for (i = 0; i < size; i++)
			sum += rom[bit + i];
		if (sum & 0xff)
			continue;
		GU_NOTE("ROM BIT: offset=%#x size=%u stride=%u tokens=%u adjust=%#x\n",
			bit, size, stride, tokens, adjust);
		if (range(length, (u64)bit + size, (u64)tokens * stride))
			return GU_DIAG(-EINVAL);
		for (i = 0; i < tokens; i++) {
			const u8 *token = rom + bit + size + i * stride;
			u32 ptr, j;
			u64 table;

			if (token[0] != 0x70 || token[1] != 2 || r16(token + 2) < 4)
				continue;
			ptr = stride >= 8 ? r32(token + 4) : r16(token + 4);
			if (range(length, ptr, r16(token + 2)))
				return GU_DIAG(-EINVAL);
			table = (u64)adjust + r32(rom + ptr);
			if (range(length, table, 6))
				return GU_DIAG(-EINVAL);
			GU_NOTE("ROM ucode table: offset=%#llx version=%u header=%u stride=%u entries=%u\n",
				(unsigned long long)table, rom[table], rom[table+1], rom[table+2], rom[table+3]);
			if (rom[table] != 1 || rom[table + 1] < 6 || rom[table + 2] < 6)
				return GU_DIAG(-EOPNOTSUPP);
			if (range(length, table + rom[table + 1],
				  (u64)rom[table + 2] * rom[table + 3]))
				return GU_DIAG(-EINVAL);
			for (j = 0; j < rom[table + 3]; j++) {
				const u8 *entry = rom + table + rom[table + 1] + j * rom[table + 2];

				if (entry[0] == 0x85)
					return GU_DIAG(descriptor(rom, length,
							  (u64)adjust + r32(entry + 2), out));
			}
		}
	}
	return GU_DIAG(-ENOENT);
}

int gpu_unlock_fwsec_sig_offset(u32 versions, u32 fuse_version, u32 sig_size, u32 *offset)
{
	u32 mask, n = 0;

	GU_NOTE("FWSEC signature selection: versions=%#x fuse_version=%u sig_bytes=%u\n",
		versions, fuse_version, sig_size);
	if (!offset || !sig_size || fuse_version >= 16 || versions > 0xffff)
		return GU_DIAG(-EINVAL);
	if (!(versions & (1u << fuse_version)))
		return GU_DIAG(-ERANGE);
	mask = versions & ((1u << fuse_version) - 1);
	while (mask) {
		n += mask & 1;
		mask >>= 1;
	}
	if (n && sig_size > ~(u32)0 / n)
		return GU_DIAG(-EOVERFLOW);
	*offset = n * sig_size;
	return 0;
}

int gpu_unlock_fwsec_patch_frts(u8 *image, const struct gpu_unlock_fwsec *fw, u64 frts_offset)
{
	u8 *dmem, *mapper = NULL;
	u32 entries, i, cmd_off, cmd_size;
	u64 off;
	u8 cmd[44] = { 0 };

	if (!image || !fw || !frts_offset || (frts_offset & 0xfff) ||
	    (frts_offset >> 12) > ~(u32)0 ||
	    range(fw->image_size, fw->data_off, fw->dmem_size) ||
	    range(fw->dmem_size, fw->interface_off, 4))
		return GU_DIAG(-EINVAL);
	dmem = image + fw->data_off;
	GU_NOTE("FWSEC interface: offset=%#x version=%u header=%u stride=%u entries=%u FRTS=%#llx\n",
		fw->interface_off, dmem[fw->interface_off], dmem[fw->interface_off+1],
		dmem[fw->interface_off+2], dmem[fw->interface_off+3], (unsigned long long)frts_offset);
	if (dmem[fw->interface_off] != 1 || dmem[fw->interface_off + 1] != 4 ||
	    dmem[fw->interface_off + 2] != 8)
		return GU_DIAG(-EOPNOTSUPP);
	entries = dmem[fw->interface_off + 3];
	off = (u64)fw->interface_off + 4;
	if (entries < 2 || range(fw->dmem_size, off, (u64)entries * 8))
		return GU_DIAG(-EINVAL);
	for (i = 0; i < entries; i++, off += 8) {
		u32 dest = r32(dmem + off + 4);

		if (r32(dmem + off) == 4) {
			if (mapper || range(fw->dmem_size, dest, 64))
				return GU_DIAG(-EINVAL);
			mapper = dmem + dest;
		}
	}
	if (!mapper)
		return GU_DIAG(-ENOENT);
	GU_NOTE("FWSEC mapper: version=%u size=%u cmd_offset=%#x cmd_size=%u\n",
		r16(mapper+4), r16(mapper+6), r32(mapper+8), r32(mapper+12));
	/* Only the audited V3 mapper layout is supported. */
	if (r16(mapper + 4) != 3 || r16(mapper + 6) < 64)
		return GU_DIAG(-EOPNOTSUPP);
	cmd_off = r32(mapper + 8);
	cmd_size = r32(mapper + 12);
	if (cmd_size < sizeof(cmd) || range(fw->dmem_size, cmd_off, cmd_size))
		return GU_DIAG(-EINVAL);
	w32(cmd, 1);
	w32(cmd + 4, 24);
	w32(cmd + 20, 2);
	w32(cmd + 24, 1);
	w32(cmd + 28, 20);
	w32(cmd + 32, frts_offset >> 12);
	w32(cmd + 36, 0x100);
	w32(cmd + 40, 2);
	memcpy(dmem + cmd_off, cmd, sizeof(cmd));
	w32(mapper + 44, 0x15);
	return 0;
}
