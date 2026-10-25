/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "../checks.h"
/* The production validator is linked with profile.c's other functions. Every
 * hardware dependency of those unused functions is a fail-fast host trap. */
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,VOID *value){(void)w;(void)off;(void)value;CHECK(0);return EFI_UNSUPPORTED;}
EFI_STATUS GuReadRebar(UINT32 values[7],BOOLEAN afterFlr){(void)values;(void)afterFlr;CHECK(0);return EFI_UNSUPPORTED;}
UINT32 GuRead32(UINT32 off){(void)off;CHECK(0);return 0;}
BOOLEAN GuEntrySelectorsMatch(UINT32 a,UINT32 b){(void)a;(void)b;CHECK(0);return FALSE;}
BOOLEAN GuLatchEntrySelectors(UINT32 a,UINT32 b){(void)a;(void)b;CHECK(0);return FALSE;}
VOID GuCopyFromIo(VOID *out,const VOID *in,size_t size){(void)out;(void)in;(void)size;CHECK(0);}
int gpu_unlock_vbios_fwsec(const u8 *rom,size_t size,struct gpu_unlock_fwsec *fw){(void)rom;(void)size;(void)fw;CHECK(0);return -EIO;}
#ifdef BOOT4070S_APP
VOID *GuAlloc(size_t n){(void)n;CHECK(0);return NULL;}
VOID GuFree(VOID *p){(void)p;CHECK(0);}
#else
BOOLEAN GuCheckConfig(const UINT32 c[64],UINTN s,UINTN b,UINTN d,UINTN f,GU_CAPS *caps){(void)c;(void)s;(void)b;(void)d;(void)f;(void)caps;CHECK(0);return FALSE;}
#endif
