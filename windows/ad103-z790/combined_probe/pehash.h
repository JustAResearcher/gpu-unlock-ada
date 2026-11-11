/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only in-memory PE32/PE32+ header walk, for hashing a loaded UEFI
 * driver's own sections. Everything is bounds-checked against the image's
 * reported size before any byte is read -- this walks a loaded firmware
 * driver image, not attacker-controlled input, but the parsing is new code
 * and gets no special trust for that. Never writes to the image; only reads
 * it and feeds ranges to GuSha256 (already reviewed, unmodified from
 * ad104-msi/sha256.c). */
#pragma once
#include <Uefi.h>

#define GU_PE_MAX_SECTIONS 64U

typedef struct {
  CHAR8 name[9]; /* 8 raw bytes + NUL we add */
  UINT32 virtual_address, virtual_size, characteristics;
  BOOLEAN looks_like_code; /* CNT_CODE && MEM_EXECUTE && !MEM_WRITE */
} GU_PE_SECTION;

typedef struct {
  BOOLEAN parsed; /* FALSE if the header didn't parse -- every other field is then unset */
  BOOLEAN is_pe32_plus;
  UINT16 machine;
  UINTN section_count;
  GU_PE_SECTION sections[GU_PE_MAX_SECTIONS];
} GU_PE_INFO;

/* Walks the DOS/PE/section headers at ImageBase (ImageBytes long). Every
 * offset is checked against ImageBytes before use; a malformed header
 * leaves Out->parsed FALSE rather than reading past the image. */
VOID GuParsePeHeaders(const VOID *ImageBase, UINTN ImageBytes, GU_PE_INFO *Out);

/* SHA-256 of ImageBase[Offset..Offset+Length), as a 64-hex-digit string in
 * HexOut (65 bytes incl. NUL). Returns FALSE (and leaves HexOut untouched)
 * if Offset+Length would read past ImageBytes. */
BOOLEAN GuHashImageRange(const VOID *ImageBase, UINTN ImageBytes, UINTN Offset, UINTN Length, char HexOut[65]);
