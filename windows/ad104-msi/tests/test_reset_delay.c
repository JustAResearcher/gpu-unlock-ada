/* SPDX-License-Identifier: GPL-2.0-only */
/* Public regression: real port/compatibility code, synthetic EFI callbacks.
 * No firmware, PROM, driver image, Windows image or hardware is used. */
#include "../port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef EPROTO
#undef EBADMSG
#undef EOVERFLOW
#undef EOPNOTSUPP
#undef ETIMEDOUT
#undef EALREADY
#include "../compat/compat.h"
/* Exercise the ownership callback's early guard without a captured graph. */
#include "../msi_release.c"

static EFI_BOOT_SERVICES Bs;
static EFI_PCI_IO_PROTOCOL Io;
static UINT8 Config[256],Buffer[4096];
static UINT32 Ss0,Ss1;
static GU_DMA Mapping;
static EFI_STATUS FirstDelay,LaterDelay,ResetStatus;
static UINT64 QuietMicros;
static unsigned Checks,Cases,TargetCalls,StallCalls,ResetWrites,PostResetReads;
static BOOLEAN RetryIdentity,RemoveLaterDelay;
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"reset-delay FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

VOID GuLog(const char *format,...) {(void)format;}
VOID GuLogText(const char *text) {(void)text;}
BOOLEAN GuBootLogOpen(VOID){return TRUE;}
/* Cold-profile admission is outside this test and must never be reached. */
BOOLEAN GuAd104ColdEntry(VOID){CHECK(FALSE);return FALSE;}
static VOID TargetAccess(VOID) {
  TargetCalls++;
  CHECK(!ResetWrites || QuietMicros>=100000);
}
static EFI_STATUS EFIAPI ConfigRead(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,
                                   UINT32 off,UINTN count,VOID *value) {
  TargetAccess();CHECK(p==&Io && count==1 && off+(1U<<w)<=sizeof(Config));
  if(ResetWrites && off==0) {
    PostResetReads++;
    if(RetryIdentity){*(UINT32 *)value=0xffffffffU;return EFI_SUCCESS;}
  }
  memcpy(value,Config+off,(size_t)1<<w);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ConfigWrite(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,
                                    UINT32 off,UINTN count,VOID *value) {
  TargetAccess();CHECK(p==&Io && count==1 && off+(1U<<w)<=sizeof(Config));
  memcpy(Config+off,value,(size_t)1<<w);
  if(off==(UINT32)Gu.pcie_cap+8U && (*(UINT16 *)value&0x8000)) {
    CHECK(!ResetWrites);ResetWrites++;
    return ResetStatus;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MemoryRead(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,
                                   UINT8 bar,UINT64 off,UINTN count,VOID *value) {
  TargetAccess();CHECK(p==&Io && !bar);
  if(w==EfiPciIoWidthUint32 && count==1) {
    *(UINT32 *)value=off==0x82381c?Ss0:off==0x823820?Ss1:0;
  } else memset(value,0,count*((UINTN)1<<w));
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MemoryWrite(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,
                                    UINT8 bar,UINT64 off,UINTN count,VOID *value) {
  TargetAccess();CHECK(p==&Io && !bar && w==EfiPciIoWidthUint32 && count==1);
  if(off==0x823820)Ss1=*(UINT32 *)value;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Attributes(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,
                                   UINT64 attributes,UINT64 *result) {
  TargetAccess();CHECK(p==&Io);(void)op;(void)attributes;
  if(result)*result=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Flush(EFI_PCI_IO_PROTOCOL *p){TargetAccess();CHECK(p==&Io);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI AllocateBuffer(EFI_PCI_IO_PROTOCOL *p,EFI_ALLOCATE_TYPE type,
                                       EFI_MEMORY_TYPE memory,UINTN pages,VOID **out,UINT64 attributes) {
  TargetAccess();CHECK(p==&Io);(void)type;(void)memory;(void)pages;(void)attributes;*out=Buffer;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Map(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_OPERATION operation,
                            VOID *cpu,UINTN *bytes,EFI_PHYSICAL_ADDRESS *dma,VOID **mapping) {
  TargetAccess();CHECK(p==&Io);(void)operation;(void)cpu;(void)bytes;*dma=0x200000;*mapping=&Mapping;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unmap(EFI_PCI_IO_PROTOCOL *p,VOID *mapping){TargetAccess();CHECK(p==&Io && mapping==&Mapping);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI FreeBuffer(EFI_PCI_IO_PROTOCOL *p,UINTN pages,VOID *cpu){TargetAccess();CHECK(p==&Io && pages==1 && cpu==Buffer);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI AllocatePool(EFI_MEMORY_TYPE type,UINTN bytes,VOID **out){(void)type;*out=malloc(bytes);return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;}
static EFI_STATUS EFIAPI FreePool(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Delay(UINTN microseconds) {
  EFI_STATUS status=StallCalls++?LaterDelay:FirstDelay;
  if(ResetWrites && microseconds==100000) {
    CHECK(Gu.reset_delay_unverified);
    /* The latch must protect this raw-config path during the wait too, before
       a failed Stall return has had a chance to set Gu.fault. */
    unsigned before=TargetCalls;
    CHECK(!GuMsiReleased() && TargetCalls==before);
  }
  if(status==EFI_SUCCESS && ResetWrites)QuietMicros+=microseconds;
  if(RemoveLaterDelay)Bs.Stall=NULL;
  return status;
}
static VOID Setup(VOID) {
  memset(&Bs,0,sizeof(Bs));memset(&Io,0,sizeof(Io));memset(Config,0,sizeof(Config));
  memset(&Plan,0,sizeof(Plan));
  Bs.Stall=Delay;Bs.AllocatePool=AllocatePool;Bs.FreePool=FreePool;
  Io.Pci.Read=ConfigRead;Io.Pci.Write=ConfigWrite;Io.Mem.Read=MemoryRead;Io.Mem.Write=MemoryWrite;
  Io.Attributes=Attributes;Io.Flush=Flush;Io.AllocateBuffer=AllocateBuffer;
  Io.FreeBuffer=FreeBuffer;Io.Map=Map;Io.Unmap=Unmap;
  GuInit(&Bs,&Io);Gu.bar_base=GU_BAR0_BASE;Gu.bar_size=0x1000000;Gu.pcie_cap=0x78;
  Gu.profile_verified=Gu.prom_verified=Gu.owned=Gu.armed=Gu.display_ok=Gu.saved=TRUE;
  /* Synthetic register/config values satisfy only GuFlr's existing guards. */
  UINT32 id=GU_BOARD_ID,cap=1U<<28;UINT16 command=3;
  memcpy(Config,&id,4);memcpy(Config+4,&command,2);memcpy(Config+0x7c,&cap,4);
  FirstDelay=LaterDelay=ResetStatus=EFI_SUCCESS;RetryIdentity=RemoveLaterDelay=FALSE;
  QuietMicros=0;TargetCalls=StallCalls=ResetWrites=PostResetReads=0;
  CHECK(GuLatchEntrySelectors(0x12345678,1));Ss0=0x88888888;Ss1=1;
  CHECK(GuSelectorPhase(GU_SELECTOR_PHASE_SS0_WRITTEN));
  CHECK(GuSelectorPhase(GU_SELECTOR_PHASE_SS1_WRITE));
  GuWrite32(0x823820,8);CHECK(GuSelectorPhase(GU_SELECTOR_PHASE_TARGET));
  Mapping=(GU_DMA){.cpu=Buffer,.mapping=&Mapping,.dma=0x200000,.bytes=4096,.pages=1,.mapped=TRUE};
  Gu.allocations=&Mapping;Gu.dma_count=1;Gu.ticks=17;
  Plan.ready=Plan.claimed=TRUE;Plan.stage=3;
}
/* A reset-delay failure must remain a barrier even to containment/raw helpers
 * and even if an unrelated caller clears the ordinary fault flag. */
static VOID CheckNoTargetAccess(VOID) {
  unsigned before=TargetCalls,stalls=StallCalls;UINT32 value=0;UINT8 bytes[16];VOID *cpu=NULL;UINT64 dma=0;
#define DENIED(call) do{Gu.fault=Gu.retained=FALSE;Gu.quiesced=TRUE;(void)(call);CHECK(TargetCalls==before && Gu.reset_delay_unverified);}while(0)
  DENIED(GuRead32(0));DENIED(GuWrite32(0,0));
  DENIED(GuConfigRead(EfiPciIoWidthUint32,0,&value));
  DENIED(GuConfigWrite(EfiPciIoWidthUint32,0,&value));
  DENIED(GuCopyFromIo(bytes,(VOID *)(UINTN)Gu.bar_base,sizeof(bytes)));
  DENIED(dma_wmb());DENIED(GuEnableMaster());DENIED(GuClearMaster());
  DENIED(GuSavePci());DENIED(GuRestorePci());DENIED(GuFlr());
  DENIED(GuDmaAllocate(4096,&cpu,&dma));DENIED(GuDmaFree(Buffer,Mapping.dma));
  CHECK(Gu.allocations==&Mapping && Gu.dma_count==1 && Mapping.mapped);
  DENIED(GuStall(100000));CHECK(StallCalls==stalls && Gu.ticks==17);
  DENIED(GuMsiReleased());
#undef DENIED
  Gu.allocations=NULL;Gu.dma_count=0;Gu.started=FALSE;Gu.fault=Gu.retained=FALSE;
  CHECK(!GuSafeToReturn());
}
static VOID PollDelays(VOID) {
  for(unsigned mode=0;mode<6;mode++) {
    Setup();Gu.allocations=NULL;Gu.dma_count=0;
    if(mode==1)FirstDelay=EFI_DEVICE_ERROR;
    if(mode==2)FirstDelay=EFI_WARN_UNKNOWN_GLYPH;
    if(mode==3)Bs.Stall=NULL;
    if(mode==4)Gu.bs=NULL;
    if(mode==5)Gu.ticks=~(UINT64)0-10;
    UINT64 before=Gu.ticks;GuStall(50);
    if(!mode)CHECK(!Gu.fault && Gu.ticks==before+50 && StallCalls==1);
    else {
      CHECK(Gu.fault && Gu.ticks==before);
      unsigned calls=TargetCalls,stalls=StallCalls;
      CHECK(readl((VOID *)(UINTN)Gu.bar_base)==0xffffffffU && TargetCalls==calls);
      GuStall(50);CHECK(Gu.ticks==before && StallCalls==stalls);
    }
    Cases++;
  }
}
int main(VOID) {
  PollDelays();
  Setup();CHECK(GuFlr()==EFI_SUCCESS && !Gu.fault && !Gu.reset_delay_unverified);
  CHECK(Gu.flrs==1 && ResetWrites==1 && StallCalls==1 && QuietMicros==100000 && Gu.ticks==100017);
  CHECK(PostResetReads==1);Cases++;
  for(unsigned mode=0;mode<5;mode++) {
    Setup();FirstDelay=mode==1?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;
    if(mode==2)Bs.Stall=NULL;
    if(mode==3)Gu.bs=NULL;
    if(mode==4)ResetStatus=EFI_DEVICE_ERROR;
    CHECK(EFI_ERROR(GuFlr()) && Gu.reset_delay_unverified && Gu.fault && Gu.retained);
    CHECK(Gu.flrs==1 && ResetWrites==1 && !PostResetReads && !QuietMicros && Gu.ticks==17);
    CHECK(StallCalls==(mode==2||mode==3?0U:1U));
    CheckNoTargetAccess();Cases++;
  }
  /* A reset callback can report failure after writing. The quiet period still
     has to run; only after its success may ordinary containment access resume. */
  Setup();ResetStatus=EFI_DEVICE_ERROR;
  CHECK(EFI_ERROR(GuFlr()) && Gu.fault && !Gu.reset_delay_unverified);
  CHECK(QuietMicros==100000 && Gu.ticks==100017 && StallCalls==1 && !PostResetReads);
  CHECK(GuClearMaster()==EFI_SUCCESS);Cases++;
  /* An identity retry must stop after its delay fails, not issue another raw
     PCI read just because the mandatory 100 ms interval already succeeded. */
  for(unsigned mode=0;mode<3;mode++) {
    Setup();RetryIdentity=TRUE;LaterDelay=mode?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;
    if(mode==2)RemoveLaterDelay=TRUE;
    CHECK(EFI_ERROR(GuFlr()) && Gu.fault && !Gu.reset_delay_unverified);
    CHECK(PostResetReads==1 && QuietMicros==100000 && Gu.ticks==100017);
    Cases++;
  }
  printf("Reset delay: %u checks, %u synthetic scenarios; no hardware or private inputs.\n",Checks,Cases);
  return 0;
}
