/* SPDX-License-Identifier: GPL-2.0-only */
/* Invented PE memory images; no private firmware or GPU access. */
#include "capture_pe.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_BYTES 4096
#define OPT 0x98
#define TABLE (OPT+240)
static UINT8 original[IMAGE_BYTES],scratch[IMAGE_BYTES+IMAGE_BYTES/8];
static GU_PE_IDENTITY identity;
static void put16(UINT8 *p,UINT16 v){p[0]=(UINT8)v;p[1]=(UINT8)(v>>8);}
static void put32(UINT8 *p,UINT32 v){put16(p,(UINT16)v);put16(p+2,(UINT16)(v>>16));}
static void put64(UINT8 *p,UINT64 v){put32(p,(UINT32)v);put32(p+4,(UINT32)(v>>32));}
static void fixture(UINT8 *p,UINT64 base){
  memset(p,0,IMAGE_BYTES);p[0]='M';p[1]='Z';put32(p+60,0x80);
  memcpy(p+0x80,"PE\0\0",4);put16(p+0x84,0x8664);put16(p+0x86,3);
  put16(p+0x94,240);put16(p+OPT,0x20b);put32(p+OPT+16,0x210);
  put64(p+OPT+24,0x100000);put32(p+OPT+56,IMAGE_BYTES);put32(p+OPT+60,0x200);
  put32(p+OPT+108,6);put32(p+OPT+112+40,0x600);put32(p+OPT+116+40,12);
  const char *names[]={".text",".data",".reloc"};
  const UINT32 starts[]={0x200,0x400,0x600},sizes[]={0x200,0x100,0x100};
  for(unsigned i=0;i<3;i++){
    UINT8 *s=p+TABLE+i*40;memcpy(s,names[i],strlen(names[i]));
    put32(s+8,sizes[i]);put32(s+12,starts[i]);put32(s+16,sizes[i]);
    put32(s+36,i==0?0x60000020:0x40000040);
  }
  for(unsigned i=0x200;i<0x400;i++)p[i]=(UINT8)(i*17+3);
  put64(p+0x240,base+0x350);put64(p+0x420,base+0x430);
  put32(p+0x600,0);put32(p+0x604,12);put16(p+0x608,0xa240);put16(p+0x60a,0xa420);
}
static EFI_STATUS run(UINT8 *p,UINTN n,UINT64 base,UINTN stop){
  UINT8 saved[IMAGE_BYTES];assert(n<=IMAGE_BYTES);memcpy(saved,p,n);
  memset(&identity,0xa5,sizeof(identity));
  EFI_STATUS s=GuPeIdentity(p,n,base,stop,scratch,sizeof(scratch),&identity);
  assert(!memcmp(saved,p,n));
  if(EFI_ERROR(s) && n)assert(!identity.canonical_valid);
  return s;
}
static void valid_and_normalization(void){
  UINT8 second[IMAGE_BYTES],hash[32],raw[32],headers[32],relocs[32];fixture(original,0x300000);
  assert(run(original,IMAGE_BYTES,0x300000,0x220)==EFI_SUCCESS);
  assert(identity.image_hash_valid && identity.headers_valid && identity.canonical_valid);
  assert(identity.section_count==3 && identity.stop_section==0);
  assert(identity.sections[0].rva==0x200 && identity.sections[0].bytes==0x200);
  assert(identity.relocation_count==2 && identity.executable_relocations==1);
  assert(identity.sections[0].relocations==1 && identity.sections[1].relocations==1);
  memcpy(hash,identity.sections[0].canonical_sha256,32);memcpy(raw,identity.sections[0].raw_sha256,32);
  memcpy(headers,identity.headers_sha256,32);memcpy(relocs,identity.relocation_sha256,32);
  fixture(second,0x500000);
  assert(run(second,IMAGE_BYTES,0x500000,0x220)==EFI_SUCCESS);
  assert(!memcmp(hash,identity.sections[0].canonical_sha256,32));
  assert(memcmp(raw,identity.sections[0].raw_sha256,32));
  assert(!memcmp(headers,identity.headers_sha256,32));
  assert(!memcmp(relocs,identity.relocation_sha256,32));
  fixture(second,0x100000);
  assert(run(second,IMAGE_BYTES,0x100000,0x220)==EFI_SUCCESS);
  assert(!memcmp(hash,identity.sections[0].raw_sha256,32));
  assert(!memcmp(hash,identity.sections[0].canonical_sha256,32));
}
static void rejection_cases(void){
  for(unsigned test=0;test<27;test++){
    fixture(original,0x300000);UINTN stop=0x220;
    switch(test){
    case 0:original[0]='V';original[1]='Z';break;
    case 1:put32(original+60,0xfffffffe);break;
    case 2:put16(original+0x84,0x14c);break;
    case 3:put16(original+0x86,0);break;
    case 4:put16(original+0x86,33);break;
    case 5:put16(original+0x94,100);break;
    case 6:put16(original+OPT,0x10b);break;
    case 7:put32(original+OPT+56,IMAGE_BYTES+1);break;
    case 8:put32(original+OPT+60,0x100);break;
    case 9:put32(original+OPT+108,0xffffffff);break;
    case 10:put32(original+OPT+112+40,IMAGE_BYTES-4);break;
    case 11:put32(original+OPT+116+40,0);break;
    case 12:put32(original+TABLE+12,0x180);break;
    case 13:put32(original+TABLE+8,IMAGE_BYTES);break;
    case 14:put32(original+TABLE+40+12,0x300);break;
    case 15:stop=0x420;break;
    case 16:stop=0x400;break;
    case 17:put32(original+OPT+16,0x420);break;
    case 18:put32(original+0x604,7);break;
    case 19:put32(original+0x604,14);break;
    case 20:put32(original+0x600,1);break;
    case 21:put16(original+0x608,0x3240);break;
    case 22:put16(original+0x608,0xa3fc);break;
    case 23:put16(original+0x60a,0xa240);break;
    case 24:put16(original+0x60a,0xa244);break;
    case 25:put16(original+0x608,0xa608);break;
    case 26:put16(original+0x608,0xa100);break;
    }
    EFI_STATUS s=run(original,IMAGE_BYTES,0x300000,stop);
    if(!EFI_ERROR(s)){fprintf(stderr,"unexpected success in case %u\n",test);abort();}
  }
}
static void absence_and_padding(void){
  fixture(original,0x300000);put16(original+0x60a,0); /* ABSOLUTE padding */
  assert(run(original,IMAGE_BYTES,0x300000,0x220)==EFI_SUCCESS);
  assert(identity.relocation_count==1 && identity.executable_relocations==1);
  fixture(original,0x100000);put32(original+OPT+112+40,0);put32(original+OPT+116+40,0);
  assert(run(original,IMAGE_BYTES,0x100000,0x220)==EFI_SUCCESS);
  assert(!memcmp(identity.sections[0].raw_sha256,identity.sections[0].canonical_sha256,32));
  assert(run(original,IMAGE_BYTES,0x300000,0x220)==EFI_UNSUPPORTED);
  fixture(original,0x300000);put32(original+TABLE+8,0); /* Raw size fallback */
  assert(run(original,IMAGE_BYTES,0x300000,0x220)==EFI_SUCCESS);
}
static void boundaries_and_no_alias_writes(void){
  fixture(original,0x300000);
  for(UINTN size=1;size<IMAGE_BYTES;size++){
    UINT8 *exact=malloc(size);assert(exact);memcpy(exact,original,size);
    assert(EFI_ERROR(run(exact,size,0x300000,0x220)));free(exact);
  }
  UINT8 saved[IMAGE_BYTES];memcpy(saved,original,sizeof(saved));
  assert(GuPeIdentity(original,IMAGE_BYTES,0x300000,0x220,original,sizeof(original),&identity)==EFI_INVALID_PARAMETER);
  assert(GuPeIdentity(original,IMAGE_BYTES,0x300000,0x220,scratch,sizeof(scratch),(VOID*)original)==EFI_INVALID_PARAMETER);
  assert(GuPeIdentity(original,IMAGE_BYTES,0x300000,0x220,scratch,sizeof(scratch),(VOID*)scratch)==EFI_INVALID_PARAMETER);
  assert(GuPeIdentity(original,IMAGE_BYTES,0x300000,0x220,scratch,GuPeScratchBytes(IMAGE_BYTES)-1,&identity)==EFI_INVALID_PARAMETER);
  assert(GuPeIdentity(original,GU_PE_MAX_IMAGE+1,0x300000,0x220,scratch,sizeof(scratch),&identity)==EFI_INVALID_PARAMETER);
  assert(!memcmp(saved,original,sizeof(saved)));
  assert(GuPeScratchBytes(0)==0 && GuPeScratchBytes(GU_PE_MAX_IMAGE+1)==0);
}
int main(void){
  valid_and_normalization();rejection_cases();absence_and_padding();boundaries_and_no_alias_writes();
  puts("PE capture: normalization, 27 malformed cases, 4095 truncations, alias/source preservation: PASS");
  return 0;
}
