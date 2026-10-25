/* SPDX-License-Identifier: GPL-2.0-only */
/* PE fields and IMAGE_REL_BASED_DIR64 follow Microsoft's PE/COFF specification.
 * All reads are byte-based and bounded; normalization touches a private copy. */
#include "capture_pe.h"
#include "runtime.h"
#include "sha256.h"

static UINT16 U16(const UINT8 *p) {return (UINT16)(p[0] | ((UINT16)p[1]<<8));}
static UINT32 U32(const UINT8 *p) {return (UINT32)U16(p) | ((UINT32)U16(p+2)<<16);}
static UINT64 U64(const UINT8 *p) {return (UINT64)U32(p) | ((UINT64)U32(p+4)<<32);}
static VOID Put64(UINT8 *p,UINT64 v) {for(UINTN i=0;i<8;i++){p[i]=(UINT8)v;v>>=8;}}
static BOOLEAN Span(UINTN at,UINTN n,UINTN size) {return at<=size && n<=size-at;}
static BOOLEAN Overlap(const VOID *a,UINTN an,const VOID *b,UINTN bn) {
  UINTN x=(UINTN)a,y=(UINTN)b;
  if(an>~(UINTN)0-x || bn>~(UINTN)0-y)return TRUE;
  return an && bn && x<y+bn && y<x+an;
}
UINTN GuPeScratchBytes(UINTN Bytes) {
  return Bytes && Bytes<=GU_PE_MAX_IMAGE?Bytes+(Bytes+7)/8:0;
}
static INTN SectionAt(const GU_PE_IDENTITY *o,UINTN rva,UINTN bytes) {
  for(UINTN i=0;i<o->section_count;i++) {
    const GU_PE_SECTION *s=&o->sections[i];
    if(rva>=s->rva && Span(rva-s->rva,bytes,s->bytes))return (INTN)i;
  }
  return -1;
}
static EFI_STATUS Relocations(const UINT8 *image,UINT8 *copy,UINT8 *bitmap,
                              GU_PE_IDENTITY *o) {
  if(!o->relocation_bytes)return EFI_SUCCESS;
  UINTN cursor=o->relocation_rva,end=cursor+o->relocation_bytes;
  UINT64 delta=o->loaded_base-o->preferred_base;
  UINTN entries_seen=0,blocks_seen=0;
  while(cursor<end) {
    if(++blocks_seen>GU_PE_MAX_RELOC_BLOCKS)return EFI_BAD_BUFFER_SIZE;
    if(!Span(cursor,8,end))return EFI_COMPROMISED_DATA;
    UINT32 page=U32(image+cursor),bytes=U32(image+cursor+4);
    if((page&0xfff) || bytes<8 || (bytes&3) || !Span(cursor,bytes,end))return EFI_COMPROMISED_DATA;
    for(UINTN at=cursor+8;at<cursor+bytes;at+=2) {
      UINT16 entry=U16(image+at);UINTN type=entry>>12;
      if(++entries_seen>GU_PE_MAX_RELOCATIONS)return EFI_BAD_BUFFER_SIZE;
      if(!type)continue; /* ABSOLUTE entries are padding, never fixups. */
      if(type!=10)return EFI_UNSUPPORTED;
      UINTN rva=(UINTN)page+(entry&0xfff);
      if(!Span(rva,8,o->image_bytes))return EFI_COMPROMISED_DATA;
      INTN section=SectionAt(o,rva,8);
      if(section<0)return EFI_COMPROMISED_DATA;
      /* Refuse self-modifying relocation tables and every duplicate/overlap. */
      if(rva<end && o->relocation_rva<rva+8)return EFI_COMPROMISED_DATA;
      for(UINTN n=rva;n<rva+8;n++) {
        UINT8 mask=(UINT8)(1U<<(n&7));
        if(bitmap[n>>3]&mask)return EFI_COMPROMISED_DATA;
        bitmap[n>>3]|=mask;
      }
      o->relocation_count++;
      GU_PE_SECTION *s=&o->sections[section];s->relocations++;
      if(s->executable) {
        o->executable_relocations++;
        Put64(copy+rva,U64(image+rva)-delta);
      }
    }
    cursor+=bytes;
  }
  return EFI_SUCCESS;
}

EFI_STATUS GuPeIdentity(const UINT8 *Image,UINTN Bytes,UINT64 LoadedBase,
                       UINTN StopOffset,VOID *Scratch,UINTN ScratchBytes,
                       GU_PE_IDENTITY *Out) {
  UINTN needed=GuPeScratchBytes(Bytes);
  if(!Image || !Out || !Scratch || !needed || ScratchBytes<needed ||
     Overlap(Image,Bytes,Scratch,needed) || Overlap(Image,Bytes,Out,sizeof(*Out)) ||
     Overlap(Scratch,needed,Out,sizeof(*Out)))return EFI_INVALID_PARAMETER;
  memset(Out,0,sizeof(*Out));Out->stop_section=0xffffffffU;
  GuSha256(Image,Bytes,Out->image_sha256);Out->image_hash_valid=TRUE;
  Out->loaded_base=LoadedBase;
  if(Bytes<64 || Image[0]!='M' || Image[1]!='Z')return EFI_UNSUPPORTED;
  UINTN pe=U32(Image+60);
  if(pe<64 || !Span(pe,24,Bytes) || memcmp(Image+pe,"PE\0\0",4))return EFI_COMPROMISED_DATA;
  if(U16(Image+pe+4)!=0x8664)return EFI_UNSUPPORTED;
  UINTN sections=U16(Image+pe+6),optional=U16(Image+pe+20),opt=pe+24;
  if(!sections || sections>GU_PE_MAX_SECTIONS || optional<112 ||
     !Span(opt,optional,Bytes))return EFI_COMPROMISED_DATA;
  if(U16(Image+opt)!=0x20b)return EFI_UNSUPPORTED;
  UINTN table=opt+optional;
  if(!Span(table,sections*40,Bytes))return EFI_COMPROMISED_DATA;
  Out->image_bytes=U32(Image+opt+56);Out->headers_bytes=U32(Image+opt+60);
  Out->entry_rva=U32(Image+opt+16);Out->preferred_base=U64(Image+opt+24);
  if(!Out->image_bytes || Out->image_bytes>Bytes || Out->headers_bytes<table+sections*40 ||
     Out->headers_bytes>Out->image_bytes || StopOffset>=Out->image_bytes ||
     LoadedBase>~(UINT64)0-Out->image_bytes)return EFI_COMPROMISED_DATA;
  UINTN directories=U32(Image+opt+108);
  if(directories>(optional-112)/8)return EFI_COMPROMISED_DATA;
  if(directories>5) {
    Out->relocation_rva=U32(Image+opt+112+5*8);
    Out->relocation_bytes=U32(Image+opt+116+5*8);
    if((!Out->relocation_rva)!=(!Out->relocation_bytes) ||
       !Span(Out->relocation_rva,Out->relocation_bytes,Out->image_bytes))return EFI_COMPROMISED_DATA;
  }
  /* A stripped relocation directory cannot establish a normalized identity at
   * another base. With no relocation directory, raw code hashes are valid only
   * if the image was loaded at its preferred base; canonical_valid stays false
   * otherwise and the caller receives an explicit unsupported result. */
  if(!Out->relocation_bytes && LoadedBase!=Out->preferred_base)return EFI_UNSUPPORTED;
  Out->section_count=(UINT32)sections;
  for(UINTN i=0;i<sections;i++) {
    const UINT8 *header=Image+table+i*40;GU_PE_SECTION *s=&Out->sections[i];
    for(UINTN j=0;j<8;j++)s->name[j]=(header[j]>=32 && header[j]<=126)?(char)header[j]:'_';
    s->name[8]=0;s->bytes=U32(header+8);s->rva=U32(header+12);
    if(!s->bytes)s->bytes=U32(header+16);
    s->characteristics=U32(header+36);s->executable=!!(s->characteristics&0x20000000U);
    if(!s->bytes || s->rva<Out->headers_bytes || !Span(s->rva,s->bytes,Out->image_bytes))return EFI_COMPROMISED_DATA;
    for(UINTN j=0;j<i;j++) {
      GU_PE_SECTION *prior=&Out->sections[j];
      if(s->rva<(UINT64)prior->rva+prior->bytes && prior->rva<(UINT64)s->rva+s->bytes)return EFI_COMPROMISED_DATA;
    }
    if(s->executable) {
      GuSha256(Image+s->rva,s->bytes,s->raw_sha256);
      if(StopOffset>=s->rva && StopOffset-s->rva<s->bytes)Out->stop_section=(UINT32)i;
    }
  }
  if(Out->stop_section==0xffffffffU || SectionAt(Out,Out->entry_rva,1)<0)return EFI_COMPROMISED_DATA;
  INTN entry_section=SectionAt(Out,Out->entry_rva,1);
  if(!Out->sections[entry_section].executable)return EFI_COMPROMISED_DATA;
  if(Out->relocation_bytes && SectionAt(Out,Out->relocation_rva,Out->relocation_bytes)<0)return EFI_COMPROMISED_DATA;
  GuSha256(Image,Out->headers_bytes,Out->headers_sha256);
  GuSha256(Image+Out->relocation_rva,Out->relocation_bytes,Out->relocation_sha256);
  Out->headers_valid=TRUE;
  UINT8 *copy=Scratch,*bitmap=copy+Bytes;
  memcpy(copy,Image,Bytes);memset(bitmap,0,(Bytes+7)/8);
  EFI_STATUS status=Relocations(Image,copy,bitmap,Out);
  if(EFI_ERROR(status))return status;
  for(UINTN i=0;i<sections;i++)if(Out->sections[i].executable)
    GuSha256(copy+Out->sections[i].rva,Out->sections[i].bytes,Out->sections[i].canonical_sha256);
  Out->canonical_valid=TRUE;return EFI_SUCCESS;
}
