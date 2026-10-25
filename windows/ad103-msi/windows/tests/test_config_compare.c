/* SPDX-License-Identifier: GPL-2.0-only */
/* CPU-only comparison fixtures. Never reads or writes PCI/firmware state. */
#include "config_compare.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

GU_PLATFORM Gu;
static unsigned comparisons;
static GU_DMA dma;
BOOLEAN GuTargetConfig(const UINT32 *c,UINTN segment,UINTN bus,UINTN device,UINTN function){
  return c&&c[0]==GU_BOARD_ID&&c[11]==GU_BOARD_SUBSYSTEM&&segment==GU_PROFILE_SEGMENT&&
    bus==GU_PROFILE_BUS&&device==GU_PROFILE_DEVICE&&function==GU_PROFILE_FUNCTION&&
    (c[2]>>24)==3&&!((c[3]>>16)&0x7f);
}
static void baseline(UINT32 c[64]){
  memset(c,0,256);c[0]=GU_BOARD_ID;c[1]=0x00100003;c[2]=0x030000a1;c[4]=0x83000000;
  c[11]=GU_BOARD_SUBSYSTEM;c[0x34/4]=0x60;c[0x60/4]=0x00037801;
  c[0x78/4]=0x00020010;c[0x7c/4]=1U<<28;c[0x80/4]=0x00092930;
}
static void clean(void){
  memset(&Gu,0,sizeof(Gu));Gu.started=Gu.saved=Gu.quiesced=TRUE;
  Gu.flrs=1;Gu.mmio_writes=17;Gu.pm_cap=0x60;Gu.pcie_cap=0x78;Gu.pcie_version=2;
}
static void check(const UINT32 a[64],const UINT32 b[64],UINT16 cap,BOOLEAN expected,UINT16 cleared){
  UINT32 original_a[64],original_b[64];memcpy(original_a,a,256);memcpy(original_b,b,256);
  GU_PLATFORM original=Gu;UINT16 actual_cleared=0xffff;
  BOOLEAN result=GuConfigEqual(a,b,cap,&actual_cleared);comparisons++;
  assert(result==expected&&actual_cleared==(expected?cleared:0));
  assert(!memcmp(a,original_a,256)&&!memcmp(b,original_b,256)&&!memcmp(&Gu,&original,sizeof(Gu)));
}
static void status_matrix(void){
  UINT32 a[64],b[64];clean();baseline(a);baseline(b);
  /* Every combination of the six defined low status bits, including pending,
   * auxiliary power and all four errors, is covered in both directions. */
  for(UINT32 before=0;before<64;before++)for(UINT32 after=0;after<64;after++){
    a[32]=0x2930|(before<<16);b[32]=0x2930|(after<<16);
#if GU_STAGED_UNLOCK
    BOOLEAN accepted=!(after&0x2f)&&!((before^after)&~9U)&&!(after&~before);
    check(a,b,0x78,accepted,accepted?(UINT16)(before&~after):0);
#else
    check(a,b,0x78,before==after,0);
#endif
  }
  baseline(a);baseline(b);b[32]=0x2930;
#if GU_STAGED_UNLOCK
  check(a,b,0x78,TRUE,9); /* Exact reported mixed control/status pair. */
  check(a,a,0x78,FALSE,0); /* Clearing then reassertion cannot hide in baseline equality. */
#else
  check(a,b,0x78,FALSE,0);check(a,a,0x78,TRUE,0);
#endif
}
static void every_other_bit(void){
  UINT32 a[64],b[64];clean();baseline(a);a[32]=0x2930;
  for(UINTN i=0;i<64;i++)for(UINTN bit=0;bit<32;bit++){
    memcpy(b,a,256);b[i]^=1U<<bit;check(a,b,0x78,FALSE,0);
  }
  /* Reserved/auxiliary status bits may only remain exactly unchanged. */
  for(UINTN bit=4;bit<16;bit++)if(bit!=5){
    baseline(a);a[32]|=1U<<(bit+16);memcpy(b,a,256);b[32]&=~0x90000U;
#if GU_STAGED_UNLOCK
    check(a,b,0x78,TRUE,9);
#else
    check(a,b,0x78,FALSE,0);
#endif
    b[32]^=1U<<(bit+16);check(a,b,0x78,FALSE,0);
  }
}
static void lifecycle(void){
  UINT32 a[64],b[64];baseline(a);memcpy(b,a,256);b[32]=0x2930;
  for(unsigned invalid=0;invalid<13;invalid++){
    clean();
    switch(invalid){
    case 0:Gu.started=FALSE;break;case 1:Gu.saved=FALSE;break;case 2:Gu.quiesced=FALSE;break;
    case 3:Gu.flrs=0;break;case 4:Gu.flrs=2;break;case 5:Gu.mmio_writes=0;break;
    case 6:Gu.fault=TRUE;break;case 7:Gu.retained=TRUE;break;case 8:Gu.dma_count=1;break;
    case 9:Gu.allocations=&dma;break;case 10:Gu.master_attributes_pending=TRUE;break;
    case 11:Gu.master_attribute_status=EFI_WARN_UNKNOWN_GLYPH;break;
    case 12:Gu.master_hardware_status=EFI_DEVICE_ERROR;break;
    }
    check(a,b,0x78,FALSE,0);
    /* Outside the eligible phase this helper remains byte-exact; callers
     * independently reject unsafe transaction state. */
    check(a,a,0x78,TRUE,0);
  }
}
static void capabilities(void){
  UINT32 a[64],b[64];
  for(unsigned variant=0;variant<23;variant++){
    clean();baseline(a);UINT16 supplied=0x78;
    switch(variant){
    case 0:supplied=0;break;case 1:supplied=0x3c;break;case 2:supplied=0x79;break;
    case 3:supplied=0xf0;break;case 4:supplied=0xffff;break;
    case 5:Gu.pcie_cap=0x80;break;case 6:Gu.pcie_version=1;break;
    case 7:a[1]&=~0x100000U;break;case 8:a[13]=0;break;case 9:a[13]=0x3c;break;
    case 10:a[13]=0x61;break;case 11:a[24]=0x00036001;break; /* cycle */
    case 12:a[24]=0x0003fc01;break; /* truncated chain */
    case 13:a[30]=0x000200ff;break; /* unknown ID */
    case 14:a[30]=0x00000010;break; /* version zero */
    case 15:a[30]=0x00030010;break; /* unknown version */
    case 16:a[30]=0x00420010;break; /* not an endpoint */
    case 17:a[31]&=~(1U<<28);break; /* no FLR support */
    case 18:a[30]=0x00028010;a[32]=0x00020010;break; /* duplicate/overlap */
    case 19:a[24]=0x00037805;break; /* PM missing */
    case 20:a[11]^=1;break;case 21:a[0]^=1;break;case 22:a[2]=0x020000a1;break;
    }
    memcpy(b,a,256);b[32]&=~0x90000U;
    /* Guarantee a raw difference even when corruption overlapped status. */
    a[32]|=0x90000U;
    check(a,b,supplied,FALSE,0);
    a[32]&=~0x90000U;
    check(a,a,supplied,!GU_STAGED_UNLOCK,0);
  }
  /* A valid header at an unlinked offset is never trusted as a capability. */
  clean();baseline(a);memcpy(b,a,256);a[0x50/4]=0x90000;b[0x50/4]=0;
  check(a,b,0x48,FALSE,0);
}
static void capability_boundaries(void){
  UINT32 a[64],b[64];
  const UINT16 locations[]={0x48,0x78,0xc4,0xd4};
  for(UINTN at=0;at<sizeof(locations)/sizeof(*locations);at++)for(UINT8 version=1;version<=2;version++){
    clean();baseline(a);memset((UINT8*)a+0x40,0,192);
    UINT16 cap=locations[at];a[13]=0x40;a[16]=0x00030001|((UINT32)cap<<8);
    a[cap/4]=0x10|((UINT32)version<<16);a[cap/4+1]=1U<<28;a[cap/4+2]=0x00092930;
    memcpy(b,a,256);b[cap/4+2]=0x2930;Gu.pcie_cap=cap;Gu.pcie_version=version;
    BOOLEAN valid=version==1||cap<=0xc4;
    check(a,b,cap,GU_STAGED_UNLOCK&&valid,GU_STAGED_UNLOCK&&valid?9:0);
  }
  clean();baseline(a);a[32]=0x2930;
  UINT16 cleared=0xffff;
  assert(!GuConfigEqual(NULL,a,0x78,&cleared)&&!cleared);comparisons++;
  cleared=0xffff;assert(!GuConfigEqual(a,NULL,0x78,&cleared)&&!cleared);comparisons++;
  assert(GuConfigEqual(a,a,0x78,NULL));comparisons++;
}
int main(void){
  status_matrix();every_other_bit();lifecycle();capabilities();capability_boundaries();
  printf("config compare PASS staged=%u comparisons=%u no_queries=1 no_mutations=1\n",GU_STAGED_UNLOCK,comparisons);
  return 0;
}
