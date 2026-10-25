/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Uefi.h>

#define GU_PE_MAX_IMAGE (16U * 1024U * 1024U)
#define GU_PE_MAX_SECTIONS 32
#define GU_PE_MAX_RELOCATIONS 65536
#define GU_PE_MAX_RELOC_BLOCKS 4096

typedef struct {
  char name[9];
  UINT32 rva, bytes, characteristics, relocations;
  BOOLEAN executable;
  UINT8 raw_sha256[32], canonical_sha256[32];
} GU_PE_SECTION;

typedef struct {
  UINT64 preferred_base, loaded_base;
  UINT32 image_bytes, headers_bytes, entry_rva, relocation_rva, relocation_bytes;
  UINT32 section_count, relocation_count, executable_relocations, stop_section;
  BOOLEAN image_hash_valid, headers_valid, canonical_valid;
  UINT8 image_sha256[32], headers_sha256[32], relocation_sha256[32];
  GU_PE_SECTION sections[GU_PE_MAX_SECTIONS];
} GU_PE_IDENTITY;

/* Loaded AMD64 PE32+ memory, not an on-disk PE file. Caller validates readable
 * Image[Bytes] before calling. Scratch and Out must not overlap Image or each
 * other. No firmware calls, allocations, callbacks or source-memory writes.
 * Only verified DIR64 fixups are normalized; unsupported formats/fixups return
 * an error with canonical_valid false. Hashes describe observations, never trust.
 * headers_valid/canonical_valid describe bounded parsing/normalization, not
 * complete PE-loader validation or provenance. Whole-image hashes may include
 * mutable runtime data; the reads are not an atomic snapshot. Invalid-parameter
 * returns may leave Out untouched. Other error returns never set canonical_valid.
 * Output fields other than image_sha256 are usable only after EFI_SUCCESS.
 * No TE assumptions are made: TE input returns EFI_UNSUPPORTED.
 */
UINTN GuPeScratchBytes(UINTN Bytes);
EFI_STATUS GuPeIdentity(const UINT8 *Image, UINTN Bytes, UINT64 LoadedBase,
                       UINTN StopOffset, VOID *Scratch, UINTN ScratchBytes,
                       GU_PE_IDENTITY *Out);
