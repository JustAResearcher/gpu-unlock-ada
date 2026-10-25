#include "port.h"
VOID *memset(VOID *, int, size_t);
VOID *memcpy(VOID *, const VOID *, size_t);
GU_PLATFORM Gu;
/* Unexpected EFI warnings are not successful transport or ownership changes.
 * Preserve real errors, but never expose a positive warning as a success to
 * Linux-compatible callers that test only the EFI error bit. */
static EFI_STATUS Result(EFI_STATUS Status) {
  return Status==EFI_SUCCESS || EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
}
VOID GuInit(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci) {
  memset(&Gu,0,sizeof(Gu)); Gu.bs=Bs; Gu.pci=Pci;
}
BOOLEAN GuTargetConfig(const UINT32 *C, UINTN S, UINTN B, UINTN D, UINTN F) {
  if(!C || S>65535 || B>255 || D>31 || F || C[0]!=GU_BOARD_ID ||
     (C[2]>>24)!=3 || ((C[3]>>16)&0x7f))return FALSE;
#if GU_ENABLE_UNLOCK || GU_REHEARSAL
  return S==GU_PROFILE_SEGMENT && B==GU_PROFILE_BUS && D==GU_PROFILE_DEVICE &&
    F==GU_PROFILE_FUNCTION && C[11]==GU_BOARD_SUBSYSTEM;
#else
  return TRUE;
#endif
}
BOOLEAN GuSafeToReturn(VOID) {
  return !Gu.retained && !Gu.master_attributes_pending &&
    (!Gu.started || (Gu.quiesced && !Gu.fault));
}
VOID *GuAlloc(size_t N) {
  VOID *Memory=NULL;EFI_STATUS Status;
  if(!N || !Gu.bs || !Gu.bs->AllocatePool)return NULL;
  Status=Gu.bs->AllocatePool(EfiLoaderData,N,&Memory);
  if(Status!=EFI_SUCCESS || !Memory) {
    /* A non-success result does not establish that an output pointer is a
     * valid allocation. Do not dereference or free an uncertain pointer. */
    if(Memory){Gu.fault=TRUE;Gu.retained=TRUE;}
    else if(Status!=EFI_SUCCESS && !EFI_ERROR(Status))Gu.fault=TRUE;
    return NULL;
  }
  memset(Memory,0,N);return Memory;
}
VOID GuFree(VOID *Memory) {
  /* Pool scratch is CPU-only; DMA uses GuDmaFree's separate lifetime guard. */
  if(Memory && !Gu.retained && (!Gu.bs || !Gu.bs->FreePool ||
      Gu.bs->FreePool(Memory)!=EFI_SUCCESS)){Gu.fault=TRUE;Gu.retained=TRUE;}
}
VOID GuStarted(VOID) {Gu.started=TRUE;Gu.quiesced=FALSE;}
VOID GuQuiesced(VOID) {if(!Gu.fault)Gu.quiesced=TRUE;}
static EFI_STATUS RetainDma(GU_DMA *Entry,EFI_STATUS Status) {
  Entry->next=Gu.allocations;Gu.allocations=Entry;
  Gu.fault=TRUE;Gu.retained=TRUE;
  return Status==EFI_SUCCESS?EFI_DEVICE_ERROR:Result(Status);
}
EFI_STATUS GuDmaAllocate(UINTN N, VOID **C, UINT64 *D) {
#if !GU_ENABLE_UNLOCK
  return EFI_ACCESS_DENIED;
#endif
  EFI_STATUS Status; GU_DMA *Entry; UINTN Mapped;
  if(!C || !D)return EFI_INVALID_PARAMETER;
  *C=NULL;*D=0;
  if(!N || N>(128U<<20) || Gu.fault || Gu.retained || !Gu.pci || !Gu.owned ||
     !Gu.pci->AllocateBuffer || !Gu.pci->FreeBuffer || !Gu.pci->Map || !Gu.pci->Unmap)return EFI_INVALID_PARAMETER;
  Entry=GuAlloc(sizeof(*Entry));if(!Entry)return EFI_OUT_OF_RESOURCES;
  Entry->pages=(N+4095)/4096;Entry->bytes=Entry->pages*4096;
  Status=Gu.pci->AllocateBuffer(Gu.pci,AllocateAnyPages,EfiBootServicesData,
    Entry->pages,&Entry->cpu,0);
  if(Status!=EFI_SUCCESS || !Entry->cpu) {
    if(Entry->cpu)return RetainDma(Entry,Status);
    if(Status!=EFI_SUCCESS && !EFI_ERROR(Status))Gu.fault=TRUE;
    GuFree(Entry);return Status!=EFI_SUCCESS?Result(Status):EFI_OUT_OF_RESOURCES;
  }
  memset(Entry->cpu,0,Entry->bytes);Mapped=Entry->bytes;
  Status=Gu.pci->Map(Gu.pci,EfiPciIoOperationBusMasterCommonBuffer,Entry->cpu,
    &Mapped,&Entry->dma,&Entry->mapping);
  /* On any Map failure its outputs have no established contract. Keep the
   * backing storage even if it returned a plausible mapping token/address;
   * attempting Unmap or freeing it would guess at partial mapping state. */
  if(Status!=EFI_SUCCESS)return RetainDma(Entry,Status);
  Entry->mapped=TRUE;
  if(Mapped!=Entry->bytes || !Entry->dma ||
     (Entry->dma&4095) || Entry->dma>0xffffffffULL || Entry->bytes-1>0xffffffffULL-Entry->dma) {
    Status=Gu.pci->Unmap(Gu.pci,Entry->mapping);
    if(Status!=EFI_SUCCESS)return RetainDma(Entry,Status);
    Entry->mapped=FALSE;
    Status=Gu.pci->FreeBuffer(Gu.pci,Entry->pages,Entry->cpu);
    if(Status!=EFI_SUCCESS)return RetainDma(Entry,Status);
    GuFree(Entry);return EFI_UNSUPPORTED;
  }
  Entry->next=Gu.allocations;Gu.allocations=Entry;Gu.dma_count++;
  *C=Entry->cpu;*D=Entry->dma;
  if(!(Gu.dma_count%2048))GuLog("DMA buffers mapped: %u\n",Gu.dma_count);
  return EFI_SUCCESS;
}
EFI_STATUS GuDmaFree(VOID *C, UINT64 D) {
  GU_DMA **Link=&Gu.allocations,*Entry;EFI_STATUS Status;
  while(*Link && ((*Link)->cpu!=C || (*Link)->dma!=D))Link=&(*Link)->next;
  if(!*Link)return EFI_NOT_FOUND;
  Entry=*Link;
  if(Gu.retained || Gu.master_attributes_pending || (Gu.started && (!Gu.quiesced || Gu.fault))) {
    Gu.retained=TRUE;return EFI_ACCESS_DENIED;
  }
  if(Entry->mapped) {
    Status=Gu.pci->Unmap(Gu.pci,Entry->mapping);
    if(Status!=EFI_SUCCESS) {Gu.fault=TRUE;Gu.retained=TRUE;return Result(Status);}
    Entry->mapped=FALSE;
  }
  Status=Gu.pci->FreeBuffer(Gu.pci,Entry->pages,Entry->cpu);
  if(Status!=EFI_SUCCESS) {Gu.fault=TRUE;Gu.retained=TRUE;return Result(Status);}
  *Link=Entry->next;GuFree(Entry);if(Gu.dma_count)Gu.dma_count--;
  return Gu.fault||Gu.retained?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
UINT32 GuRead32(UINT32 O) {
  UINT32 Value=0xffffffffU;
  if(Gu.fault)return Value;
  if(!Gu.pci || !Gu.pci->Mem.Read || (O&3) || (UINT64)O+4>Gu.bar_size ||
     Gu.pci->Mem.Read(Gu.pci,EfiPciIoWidthUint32,0,O,1,&Value)!=EFI_SUCCESS) {
    Gu.fault=TRUE;return 0xffffffffU;
  }
  return Value;
}
VOID GuWrite32(UINT32 O, UINT32 V) {
#if !GU_ENABLE_UNLOCK
  Gu.fault=TRUE;return;
#endif
  if(Gu.fault)return;
  if(!Gu.pci || !Gu.pci->Mem.Write || !Gu.armed || !Gu.owned || !Gu.display_ok || (O&3) ||
     (UINT64)O+4>Gu.bar_size) {Gu.fault=TRUE;return;}
  Gu.mmio_writes++;
  if(Gu.pci->Mem.Write(Gu.pci,EfiPciIoWidthUint32,0,O,1,&V)!=EFI_SUCCESS)Gu.fault=TRUE;
}
static EFI_STATUS RawRead(EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 O, VOID *V) {
  if(!Gu.pci || !Gu.pci->Pci.Read || !V || W>EfiPciIoWidthUint32 || O>256-((UINT32)1<<W))return EFI_INVALID_PARAMETER;
  EFI_STATUS Status=Gu.pci->Pci.Read(Gu.pci,W,O,1,V);
  if(Status!=EFI_SUCCESS && !EFI_ERROR(Status))Gu.fault=TRUE;
  return Result(Status);
}
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 O, VOID *V) {
  EFI_STATUS Status;
  if(Gu.fault)return EFI_DEVICE_ERROR;
  Status=RawRead(W,O,V);if(Status!=EFI_SUCCESS)Gu.fault=TRUE;return Status;
}
EFI_STATUS GuConfigWrite(EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 O, VOID *V) {
#if !GU_ENABLE_UNLOCK
  return EFI_ACCESS_DENIED;
#endif
  EFI_STATUS Status;
  if(Gu.fault)return EFI_DEVICE_ERROR;
  if(!Gu.pci || !Gu.pci->Pci.Write || !V || !Gu.owned || !Gu.armed || !Gu.display_ok || W>EfiPciIoWidthUint32 ||
     O>256-((UINT32)1<<W)) {Gu.fault=TRUE;return EFI_ACCESS_DENIED;}
  Status=Gu.pci->Pci.Write(Gu.pci,W,O,1,V);
  if(Status!=EFI_SUCCESS)Gu.fault=TRUE;return Result(Status);
}
VOID GuStall(UINTN Microseconds) {
  if(!Gu.bs || !Gu.bs->Stall || Gu.bs->Stall(Microseconds)!=EFI_SUCCESS)Gu.fault=TRUE;
  Gu.ticks+=Microseconds;
}
EFI_STATUS GuEnableMaster(VOID) {
#if !GU_ENABLE_UNLOCK
  return EFI_ACCESS_DENIED;
#endif
  UINT32 Id=0;UINT16 Command=0xffff,Now=0xffff;
  UINT64 Supported=0,Before=0,After=0,Decode;
  EFI_STATUS Status=EFI_ACCESS_DENIED;
  if(Gu.fault || Gu.retained || Gu.started || Gu.master_attributes_pending ||
     !Gu.pci || !Gu.pci->Attributes || !Gu.owned || !Gu.armed || !Gu.display_ok)goto fail;
  Status=RawRead(EfiPciIoWidthUint32,0,&Id);
  if(Status!=EFI_SUCCESS || Id!=GU_BOARD_ID)goto fail;
  Status=RawRead(EfiPciIoWidthUint16,4,&Command);
  if(Status!=EFI_SUCCESS || Command==0xffff || (Command&4) || !(Command&2))goto fail;
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationSupported,0,&Supported);
  if(Status!=EFI_SUCCESS || !(Supported&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))goto fail;
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Before);
  Decode=(Command&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(Command&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  if(Status!=EFI_SUCCESS || (Before&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) ||
     (Before&(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY))!=Decode)goto fail;
  GuLog("Bus Master enable intent: command=%#x attributes=%#llx\n",
    Command,(unsigned long long)Before);
  if(Gu.fault){Status=EFI_DEVICE_ERROR;goto fail;}
  Gu.master_original_attributes=Before;
  Gu.master_original_command=Command;
  Gu.master_pci=Gu.pci;
  /* Track before the call: an error may be returned after a hardware change. */
  Gu.master_attributes_pending=TRUE;
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationEnable,EFI_PCI_IO_ATTRIBUTE_BUS_MASTER,NULL);
  if(Status!=EFI_SUCCESS)goto fail;
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&After);
  if(Status!=EFI_SUCCESS || After!=(Before|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))goto fail;
  Status=RawRead(EfiPciIoWidthUint16,4,&Now);
  if(Status!=EFI_SUCCESS || Now!=(UINT16)(Command|4))goto fail;
  GuLog("Bus Master enabled: command=%#x attributes=%#llx matched=1\n",
    Now,(unsigned long long)After);
  if(Gu.fault){Status=EFI_DEVICE_ERROR;goto fail;}
  return EFI_SUCCESS;
fail:
  Gu.fault=TRUE;
  Status=Status==EFI_SUCCESS?EFI_DEVICE_ERROR:Result(Status);
  GuLog("Bus Master enable failed: status=%#llx command=%#x readback=%#x managed=%u\n",
    (unsigned long long)Status,Command,Now,Gu.master_attributes_pending);
  return Status;
}
EFI_STATUS GuClearMaster(VOID) {
#if !GU_ENABLE_UNLOCK
  return EFI_ACCESS_DENIED;
#endif
  UINT32 Id=0;UINT16 Command=0xffff,Now=0xffff;EFI_STATUS Status;
  EFI_STATUS AttributeStatus=EFI_SUCCESS;UINT64 Attributes=~0ULL;
  /* Containment remains permitted after a latched I/O failure. */
  Status=RawRead(EfiPciIoWidthUint32,0,&Id);
  if(Status!=EFI_SUCCESS || Id!=GU_BOARD_ID)goto fail;
  if(Gu.master_attributes_pending) {
    if(Gu.pci!=Gu.master_pci)AttributeStatus=EFI_COMPROMISED_DATA;
    else if(!Gu.pci->Attributes)AttributeStatus=EFI_UNSUPPORTED;
    else {
      AttributeStatus=Result(Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationDisable,
        EFI_PCI_IO_ATTRIBUTE_BUS_MASTER,NULL));
      EFI_STATUS ReadStatus=Result(Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Attributes));
      if(AttributeStatus==EFI_SUCCESS && ReadStatus!=EFI_SUCCESS)AttributeStatus=ReadStatus;
      if(AttributeStatus==EFI_SUCCESS && Attributes!=Gu.master_original_attributes)
        AttributeStatus=EFI_DEVICE_ERROR;
    }
    Gu.master_last_attributes=Attributes;
    if(AttributeStatus!=EFI_SUCCESS && Gu.master_attribute_status==EFI_SUCCESS)
      Gu.master_attribute_status=AttributeStatus;
  }
  /* Always check/clear hardware even if the protocol call failed or lied. */
  Status=RawRead(EfiPciIoWidthUint16,4,&Command);
  if(Status!=EFI_SUCCESS || Command==0xffff)goto fail;
  if(Command&4) {
    Command&=(UINT16)~4U;
    Status=Gu.pci->Pci.Write?Result(Gu.pci->Pci.Write(Gu.pci,EfiPciIoWidthUint16,4,1,&Command)):EFI_UNSUPPORTED;
    if(Status!=EFI_SUCCESS)goto fail;
  }
  Status=RawRead(EfiPciIoWidthUint16,4,&Now);
  if(Status!=EFI_SUCCESS || Now==0xffff || (Now&4))goto fail;
  /* Do not silently repair unrelated bits changed by the bus driver's API. */
  if(Gu.master_attributes_pending && Now!=Gu.master_original_command)goto fail;
  Gu.master_last_command=Now;
  if(AttributeStatus!=EFI_SUCCESS) {
    Gu.fault=TRUE;Gu.retained=TRUE;return AttributeStatus;
  }
  Gu.master_attributes_pending=FALSE;
  return EFI_SUCCESS;
fail:
  Gu.master_last_command=Now;
  Status=Status==EFI_SUCCESS?EFI_DEVICE_ERROR:Result(Status);
  if(Gu.master_hardware_status==EFI_SUCCESS)Gu.master_hardware_status=Status;
  Gu.fault=TRUE;Gu.retained=TRUE;return Status;
}
EFI_STATUS GuSavePci(VOID) {
  UINTN I;
  if(Gu.saved || Gu.fault || !Gu.owned || Gu.pcie_cap<0x40 || Gu.pcie_cap>0xd4)return EFI_NOT_READY;
  for(I=0;I<64;I++)
    if(GuConfigRead(EfiPciIoWidthUint32,(UINT32)I*4,&Gu.saved_config[I])!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
  if(Gu.saved_config[0]!=GU_BOARD_ID || Gu.saved_config[11]!=GU_BOARD_SUBSYSTEM ||
     (Gu.saved_config[1]&4))return EFI_ACCESS_DENIED;
  if(GuConfigRead(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Gu.saved_devctl)!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
  if(Gu.saved_devctl&0x8000)return EFI_NOT_READY;
  if(Gu.pcie_version>=2 && GuConfigRead(EfiPciIoWidthUint16,
     Gu.pcie_cap+0x28,&Gu.saved_devctl2)!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
  Gu.saved=TRUE;return EFI_SUCCESS;
}
EFI_STATUS GuFlr(VOID) {
  UINT32 Identity=0,Capability=0;UINT16 Command=0xffff,Pending=0xffff,Ctl=0;
  UINTN Wait;EFI_STATUS ResetStatus;
  if(!Gu.saved || Gu.flrs || Gu.fault || !Gu.owned || !Gu.armed || !Gu.display_ok)return EFI_NOT_READY;
  if(GuConfigRead(EfiPciIoWidthUint32,0,&Identity)!=EFI_SUCCESS || Identity!=GU_BOARD_ID ||
     GuConfigRead(EfiPciIoWidthUint16,4,&Command)!=EFI_SUCCESS || Command==0xffff ||
     (Command&4) || !(Command&2) ||
     GuConfigRead(EfiPciIoWidthUint32,Gu.pcie_cap+4,&Capability)!=EFI_SUCCESS ||
     !(Capability&(1U<<28)))return EFI_ACCESS_DENIED;
  for(Wait=0;Wait<=1000000;Wait+=1000) {
    if(GuConfigRead(EfiPciIoWidthUint16,Gu.pcie_cap+10,&Pending)!=EFI_SUCCESS || Pending==0xffff)return EFI_DEVICE_ERROR;
    if(!(Pending&0x20))break;
    if(Wait==1000000)return EFI_TIMEOUT;
    GuStall(1000);
  }
  if(GuConfigRead(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Ctl)!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
  if(Ctl&0x8000)return EFI_NOT_READY;
  Ctl|=0x8000;Gu.flrs++;GuStarted();
  ResetStatus=GuConfigWrite(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Ctl);
  /* No access of any kind to this function for at least 100 ms. */
  GuStall(100000);
  if(ResetStatus!=EFI_SUCCESS || Gu.fault)return EFI_DEVICE_ERROR;
  for(Wait=0;Wait<=1000000;Wait+=1000) {
    EFI_STATUS Status=RawRead(EfiPciIoWidthUint32,0,&Identity);
    if(Gu.fault)break;
    if(Status==EFI_SUCCESS && Identity==GU_BOARD_ID)return EFI_SUCCESS;
    if(Status==EFI_SUCCESS && Identity!=0xffffffffU && Identity!=0)break;
    if(Wait<1000000)GuStall(1000);
  }
  Gu.fault=TRUE;return EFI_DEVICE_ERROR;
}
EFI_STATUS GuRestorePci(VOID) {
  UINT16 Command=0,BytePair=0;UINT32 Value=0;UINT8 Line;INTN Offset;
  if(!Gu.saved || Gu.flrs!=1 || Gu.fault)return EFI_NOT_READY;
  if(GuConfigRead(EfiPciIoWidthUint16,4,&Command)!=EFI_SUCCESS || Command==0xffff || (Command&4))return EFI_DEVICE_ERROR;
  Command&=(UINT16)~7U;
  if(GuConfigWrite(EfiPciIoWidthUint16,4,&Command)!=EFI_SUCCESS ||
     GuConfigRead(EfiPciIoWidthUint16,4,&BytePair)!=EFI_SUCCESS || (BytePair&7))return EFI_DEVICE_ERROR;
  /* Decode disabled; restore high BAR dwords before their low dwords. */
  for(Offset=0x24;Offset>=0x10;Offset-=4) {
    Value=Gu.saved_config[Offset/4];
    if(GuConfigWrite(EfiPciIoWidthUint32,(UINT32)Offset,&Value)!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
  }
  Value=Gu.saved_config[0x30/4]&~1U; /* Never enable expansion ROM decoding. */
  if(GuConfigWrite(EfiPciIoWidthUint32,0x30,&Value)!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
  BytePair=(UINT16)Gu.saved_config[3];
  if(GuConfigWrite(EfiPciIoWidthUint16,0x0c,&BytePair)!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
  Line=(UINT8)Gu.saved_config[0x3c/4];
  if(GuConfigWrite(EfiPciIoWidthUint8,0x3c,&Line)!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
  if(GuConfigWrite(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Gu.saved_devctl)!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
  if(Gu.pcie_version>=2 && GuConfigWrite(EfiPciIoWidthUint16,
     Gu.pcie_cap+0x28,&Gu.saved_devctl2)!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
  Command=(UINT16)Gu.saved_config[1]&~4U;
  if(GuConfigWrite(EfiPciIoWidthUint16,4,&Command)!=EFI_SUCCESS)return EFI_DEVICE_ERROR;
  if(GuConfigRead(EfiPciIoWidthUint16,4,&BytePair)!=EFI_SUCCESS || BytePair!=Command)return EFI_DEVICE_ERROR;
  for(Offset=0x10;Offset<=0x24;Offset+=4) {
    if(GuConfigRead(EfiPciIoWidthUint32,(UINT32)Offset,&Value)!=EFI_SUCCESS ||
       Value!=Gu.saved_config[Offset/4])return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
