/* SPDX-License-Identifier: GPL-2.0-only */
/* Standalone host tool: parses the FWSEC descriptor out of a VBIOS ROM dump and prints
 * the fields profile.c needs to pin for a new board/card. Reuses the kernel module's own
 * byte parser unmodified (vbios_610.c, host/non-kernel build path) -- no kernel headers
 * needed, no device access, pure file-in/numbers-out.
 *
 * Usage:  fwsec_dump <path-to-vbios-dump.rom> [offset]
 *
 * <offset> is where the bare legacy VBIOS image starts inside the dump file (decimal or
 * 0x-prefixed hex). Default 0. A save tool (e.g. nvflash --save) can prepend its own
 * container/header before the real image -- gpu_unlock_vbios_fwsec() only ever sees
 * exactly 1 MiB starting at <offset>, matching what the EFI code reads directly from the
 * GPU's BAR0-mapped expansion-ROM window at runtime (which has no such wrapper). Do not
 * rely on offset 0 "happening to parse" on a wrapped dump -- it can land on a signature
 * that only partially overlaps the real image and silently miss or misread fields.
 *
 * Run this ONLY against your own local VBIOS dump. Keep the .rom file itself local --
 * only paste the printed fields back into the chat that asked for them. */
#include <stdio.h>
#include <stdlib.h>
#include "vbios_610.h"

#define GU_SLICE_BYTES (1024u * 1024u)

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) { fprintf(stderr, "usage: %s <vbios-dump.rom> [offset]\n", argv[0]); return 2; }
    unsigned long offset = argc == 3 ? strtoul(argv[2], NULL, 0) : 0;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    if (fseek(f, 0, SEEK_END)) { perror("seek"); fclose(f); return 1; }
    long total = ftell(f);
    if (total < 0) { perror("tell"); fclose(f); return 1; }
    if ((unsigned long)total < offset + GU_SLICE_BYTES) {
        fprintf(stderr, "file is %ld bytes, but offset 0x%lx + 1 MiB needs %lu -- wrong offset, or this is not a 1 MiB-plus dump\n",
                total, offset, offset + GU_SLICE_BYTES);
        fclose(f);
        return 1;
    }
    if (fseek(f, (long)offset, SEEK_SET)) { perror("seek to offset"); fclose(f); return 1; }

    static unsigned char rom[GU_SLICE_BYTES];
    size_t n = fread(rom, 1, sizeof(rom), f);
    fclose(f);
    if (n != GU_SLICE_BYTES) {
        fprintf(stderr, "read %zu bytes, expected exactly %u at offset 0x%lx\n", n, GU_SLICE_BYTES, offset);
        return 1;
    }

    struct gpu_unlock_fwsec fw;
    int rc = gpu_unlock_vbios_fwsec(rom, n, &fw);
    if (rc) { fprintf(stderr, "gpu_unlock_vbios_fwsec failed: %d (see gpu_unlock: failure line above)\n", rc); return 1; }

    printf("# parsed from offset 0x%lx, slice length %u\n", offset, GU_SLICE_BYTES);
    printf("image_off=0x%x\n", fw.image_off);
    printf("image_size=0x%x\n", fw.image_size);
    printf("imem_pa=0x%x\n", fw.imem_pa);
    printf("imem_size=0x%x\n", fw.imem_size);
    printf("imem_va=0x%x\n", fw.imem_va);
    printf("dmem_pa=0x%x\n", fw.dmem_pa);
    printf("data_off=0x%x\n", fw.data_off);
    printf("dmem_size=0x%x\n", fw.dmem_size);
    printf("pkc_off=0x%x\n", fw.pkc_off);
    printf("interface_off=0x%x\n", fw.interface_off);
    printf("ucode_id=%u\n", fw.ucode_id);
    printf("engine_mask=0x%x\n", fw.engine_mask);
    printf("sig_count=%u\n", fw.sig_count);
    printf("sig_versions=%u\n", fw.sig_versions);
    printf("sig_off=0x%x\n", fw.sig_off);
    printf("sig_bytes=%u\n", fw.sig_bytes);
    return 0;
}
