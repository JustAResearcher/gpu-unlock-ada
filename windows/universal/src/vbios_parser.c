/* SPDX-License-Identifier: GPL-2.0-only */
#include "vbios_parser.h"

static inline u16 Read16(const u8 *p)
{
    return (u16)p[0] | ((u16)p[1] << 8);
}

static inline u32 Read32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline int CheckRange(size_t total_len, u64 offset, u64 bytes)
{
    if (offset > total_len || bytes > total_len - offset) {
        return -1;
    }
    return 0;
}

static inline int IsRomSig(u16 sig)
{
    return sig == 0xaa55 || sig == 0x4e56 || sig == 0xbb77;
}

static inline int IsDataSig(u32 sig)
{
    /* PCIR (0x52494350), NPDS (0x5344504E), RGIS (0x53494752) */
    return sig == 0x52494350 || sig == 0x5344504e || sig == 0x53494752;
}

static int LocateExpansion(const u8 *rom, size_t length, u32 *out_adjust)
{
    u32 pos = 0, base = 0, ext = 0;
    unsigned int images = 0;

    if (CheckRange(length, 0, 2) || !IsRomSig(Read16(rom))) {
        return -1;
    }

    for (;;) {
        u64 pcir, npde;
        u32 bytes, last, sig;
        u16 ext_rev, ext_len;

        if (++images > 8 || CheckRange(length, pos, 0x1a) || !IsRomSig(Read16(rom + pos))) {
            return -1;
        }

        pcir = (u64)pos + Read16(rom + pos + 0x18);
        if (CheckRange(length, pcir, 0x16)) return -1;

        sig = Read32(rom + pcir);
        if (!IsDataSig(sig)) return -1;

        bytes = (u32)Read16(rom + pcir + 0x10) * 512;
        last = rom[pcir + 0x15] & 0x80;
        npde = (pcir + Read16(rom + pcir + 0x0a) + 15) & ~(u64)15;

        if (!CheckRange(length, npde, 4) && Read32(rom + npde) == 0x4544504e) {
            if (CheckRange(length, npde, 10)) return -1;
            ext_rev = Read16(rom + npde + 4);
            ext_len = Read16(rom + npde + 6);
            if (ext_rev == 0x100 || ext_rev == 0x101) {
                u32 sub = (u32)Read16(rom + npde + 8) * 512;
                if (ext_len < 10 || CheckRange(length, npde, ext_len)) return -1;
                if (ext_len > 10) last = rom[npde + 10] & 0x80;
                else if (sub < bytes) last = 0;
                bytes = sub;
            }
        }

        if (!bytes || CheckRange(length, pos, bytes)) return -1;
        if (!base && rom[pcir + 0x14] == 0)    base = bytes;
        if (!ext  && rom[pcir + 0x14] == 0xe0) ext = pos;
        if (last) break;
        pos += bytes;
    }

    if (ext && base && ext < base) return -1;
    *out_adjust = (ext && base) ? (ext - base) : 0;
    return 0;
}

static int ParseDescriptor(const u8 *rom, size_t length, u64 off, DYNAMIC_FWSEC *out)
{
    DYNAMIC_FWSEC fw = { 0 };
    if (CheckRange(length, off, 44)) return -1;

    u32 hdr = Read32(rom + off);
    u32 size = hdr >> 16;
    if (!(hdr & 1) || ((hdr >> 8) & 0xff) != 3 || size < 44) return -1;
    if (CheckRange(length, off, size)) return -1;

    u32 stored = Read32(rom + off + 4);
    if (!stored || stored > ~(u32)0 - 255) return -1;

    fw.image_size = (stored + 255) & ~255u;
    if (CheckRange(length, off + size, fw.image_size)) return -1;

    fw.image_off      = off + size;
    fw.pkc_off        = Read32(rom + off + 8);
    fw.interface_off  = Read32(rom + off + 12);
    fw.imem_pa        = Read32(rom + off + 16);
    fw.imem_size      = Read32(rom + off + 20);
    fw.imem_va        = Read32(rom + off + 24);
    fw.dmem_pa        = Read32(rom + off + 28);
    fw.dmem_size      = Read32(rom + off + 32);
    fw.data_off       = fw.imem_size;
    fw.engine_mask    = Read16(rom + off + 36);
    fw.ucode_id       = rom[off + 38];
    fw.sig_count      = rom[off + 39];
    fw.sig_versions   = Read16(rom + off + 40);
    fw.sig_off        = off + 44;
    fw.sig_bytes      = size - 44;

    u32 count = 0, bits = fw.sig_versions;
    while (bits) {
        count += bits & 1;
        bits >>= 1;
    }

    if (!fw.ucode_id || fw.ucode_id > 16 || !fw.engine_mask || !count ||
        count != fw.sig_count || fw.sig_bytes != count * 384 ||
        !fw.imem_size || !fw.dmem_size ||
        CheckRange(fw.image_size, fw.data_off, fw.dmem_size) ||
        CheckRange(fw.dmem_size, fw.pkc_off, 384) ||
        CheckRange(fw.dmem_size, fw.interface_off, 4)) {
        return -1;
    }

    *out = fw;
    return 0;
}

int ParseVbiosFwsec(const u8 *rom_data, size_t rom_size, DYNAMIC_FWSEC *out_fwsec)
{
    if (!rom_data || !out_fwsec || rom_size < 4096 || rom_size > 4 * 1024 * 1024) {
        return -1;
    }

    u32 adjust = 0;
    if (LocateExpansion(rom_data, rom_size, &adjust) != 0) {
        return -2;
    }

    /* Scan for BIT table: 0xb8ff followed by 'BIT\0' (0x00544942) */
    for (u32 bit = 0; bit + 12 <= rom_size; bit++) {
        if (Read16(rom_data + bit) != 0xb8ff || Read32(rom_data + bit + 2) != 0x00544942) {
            continue;
        }

        u32 size   = rom_data[bit + 8];
        u32 stride = rom_data[bit + 9];
        u32 tokens = rom_data[bit + 10];
        if (size < 12 || stride < 6 || CheckRange(rom_size, bit, size)) continue;

        u32 sum = 0;
        for (u32 k = 0; k < size; k++) sum += rom_data[bit + k];
        if (sum & 0xff) continue; /* Checksum mismatch */

        if (CheckRange(rom_size, (u64)bit + size, (u64)tokens * stride)) continue;

        for (u32 i = 0; i < tokens; i++) {
            const u8 *token = rom_data + bit + size + i * stride;
            if (token[0] != 0x70 || token[1] != 2 || Read16(token + 2) < 4) continue;

            u32 ptr = stride >= 8 ? Read32(token + 4) : Read16(token + 4);
            if (CheckRange(rom_size, ptr, Read16(token + 2))) continue;

            u64 table = (u64)adjust + Read32(rom_data + ptr);
            if (CheckRange(rom_size, table, 6)) continue;
            if (rom_data[table] != 1 || rom_data[table + 1] < 6 || rom_data[table + 2] < 6) continue;

            u32 hdr_len = rom_data[table + 1];
            u32 ent_len = rom_data[table + 2];
            u32 count   = rom_data[table + 3];

            if (CheckRange(rom_size, table + hdr_len, (u64)ent_len * count)) continue;

            for (u32 j = 0; j < count; j++) {
                const u8 *entry = rom_data + table + hdr_len + j * ent_len;
                if (entry[0] == 0x85) { /* FWSEC FRTS descriptor entry */
                    u64 desc_off = (u64)adjust + Read32(entry + 2);
                    return ParseDescriptor(rom_data, rom_size, desc_off, out_fwsec);
                }
            }
        }
    }

    return -3; /* FWSEC descriptor not found */
}

