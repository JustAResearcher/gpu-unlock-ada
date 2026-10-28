/* SPDX-License-Identifier: MIT */
#ifndef AD104_CORE_H
#define AD104_CORE_H
#include <Uefi.h>
#include "target.h"

#define PROBE_VERSION "0.4.0"
#define PROBE_DP_MAX 4096u
#define PROBE_ROM_MAX (4u * 1024u * 1024u)
typedef struct { CHAR8 *data; UINTN cap, len; BOOLEAN failed; } TEXT;
void TextInit(TEXT *t, CHAR8 *data, UINTN cap);
void TextPut(TEXT *t, const CHAR8 *s);
void TextDec(TEXT *t, UINT64 value);
void TextHex(TEXT *t, UINT64 value, UINTN digits);
void TextBytes(TEXT *t, const UINT8 *p, UINTN length);
UINT16 Read16(const UINT8 *p);
UINT32 Read32(const UINT8 *p);
UINT64 Read64(const UINT8 *p);
BOOLEAN BytesEqual(const VOID *a, const VOID *b, UINTN n);
BOOLEAN DpMeasure(const UINT8 *p, UINTN cap, UINTN *length, BOOLEAN *multi);
BOOLEAN DpParent(const UINT8 *parent, UINTN plen, const UINT8 *child, UINTN clen);
BOOLEAN DpHasUsb(const UINT8 *path, UINTN length);
BOOLEAN ExactTarget(const UINT8 *config, UINTN length);
BOOLEAN RomPcirValid(const UINT8 *rom, UINTN size, UINT16 vendor, UINT16 device);
void Sha256(const UINT8 *p, UINTN n, UINT8 result[32]);
#endif
