/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIVERSAL_VBIOS_PARSER_H
#define UNIVERSAL_VBIOS_PARSER_H

#include "uefi_base.h"

typedef struct {
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
} DYNAMIC_FWSEC;

/**
 * Parses VBIOS from memory (RAM), verifies PCIR and NPDS headers,
 * and extracts dynamic FWSEC offsets for Falcon verification.
 *
 * @param rom_data Pointer to VBIOS image in RAM.
 * @param rom_size Size of VBIOS in bytes.
 * @param out_fwsec Receives extracted FWSEC offsets and dimensions.
 * @return 0 on success, negative error code on invalid format.
 */
int ParseVbiosFwsec(const u8 *rom_data, size_t rom_size, DYNAMIC_FWSEC *out_fwsec);

#endif /* UNIVERSAL_VBIOS_PARSER_H */

