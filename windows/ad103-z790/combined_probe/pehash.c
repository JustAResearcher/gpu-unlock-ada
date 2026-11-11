/* SPDX-License-Identifier: GPL-2.0-only */
#include "pehash.h"
#include "sha256.h"

#define IMAGE_SCN_CNT_CODE 0x00000020U
#define IMAGE_SCN_MEM_EXECUTE 0x20000000U
#define IMAGE_SCN_MEM_WRITE 0x80000000U

static UINT16 Rd16(const UINT8 *p) { return (UINT16)(p[0] | ((UINT16)p[1] << 8)); }
static UINT32 Rd32(const UINT8 *p) { return (UINT32)Rd16(p) | ((UINT32)Rd16(p + 2) << 16); }

VOID GuParsePeHeaders(const VOID *ImageBase, UINTN ImageBytes, GU_PE_INFO *Out) {
  const UINT8 *b = (const UINT8 *)ImageBase;
  if (!Out) return;
  Out->parsed = FALSE;
  Out->section_count = 0;
  if (!b || ImageBytes < 0x40 || b[0] != 'M' || b[1] != 'Z') return;

  UINT32 lfanew = Rd32(b + 0x3c);
  /* Need room for the 4-byte "PE\0\0" signature plus the 20-byte COFF file
   * header right after it. */
  if (lfanew > ImageBytes || (UINT64)lfanew + 24 > ImageBytes) return;
  const UINT8 *nt = b + lfanew;
  if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) return;

  const UINT8 *file_header = nt + 4;
  UINT16 machine = Rd16(file_header + 0);
  UINT16 num_sections = Rd16(file_header + 2);
  UINT16 opt_header_size = Rd16(file_header + 16);
  if (num_sections > GU_PE_MAX_SECTIONS) return; /* real UEFI drivers have a handful, not this many */

  const UINT8 *opt_header = file_header + 20;
  UINT64 opt_end = (UINT64)(opt_header - b) + opt_header_size;
  if (opt_end > ImageBytes || opt_header_size < 2) return;
  UINT16 magic = Rd16(opt_header);
  BOOLEAN plus = magic == 0x20b;
  if (!plus && magic != 0x10b) return; /* neither PE32 nor PE32+ -- not a format we understand */

  const UINT8 *sections = opt_header + opt_header_size;
  UINT64 sections_end = (UINT64)(sections - b) + (UINT64)num_sections * 40;
  if (sections_end > ImageBytes) return;

  Out->is_pe32_plus = plus;
  Out->machine = machine;
  Out->section_count = num_sections;
  for (UINT16 i = 0; i < num_sections; i++) {
    const UINT8 *s = sections + (UINTN)i * 40;
    GU_PE_SECTION *o = &Out->sections[i];
    for (UINTN j = 0; j < 8; j++) o->name[j] = (CHAR8)s[j];
    o->name[8] = 0;
    o->virtual_size = Rd32(s + 8);
    o->virtual_address = Rd32(s + 12);
    o->characteristics = Rd32(s + 36);
    o->looks_like_code = (o->characteristics & IMAGE_SCN_CNT_CODE) &&
                          (o->characteristics & IMAGE_SCN_MEM_EXECUTE) &&
                          !(o->characteristics & IMAGE_SCN_MEM_WRITE);
  }
  Out->parsed = TRUE;
}

BOOLEAN GuHashImageRange(const VOID *ImageBase, UINTN ImageBytes, UINTN Offset, UINTN Length, char HexOut[65]) {
  static const char Alpha[] = "0123456789abcdef";
  if (!ImageBase || !HexOut || Offset > ImageBytes || Length > ImageBytes - Offset) return FALSE;
  UINT8 digest[32];
  GuSha256((const UINT8 *)ImageBase + Offset, Length, digest);
  for (UINTN i = 0; i < 32; i++) {
    HexOut[i * 2] = Alpha[digest[i] >> 4];
    HexOut[i * 2 + 1] = Alpha[digest[i] & 0xf];
  }
  HexOut[64] = 0;
  return TRUE;
}
