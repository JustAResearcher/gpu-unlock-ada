/* Modified 2026-10-19/20 for Windows GPU UNLOCK; changes documented in CHANGES.md. */ #include "port.h"
VOID *memset(VOID *, int, size_t);
VOID *memcpy(VOID *, const VOID *, size_t);
GU_PLATFORM Gu;
#ifdef GU_ENTRY_LATCHED
static BOOLEAN EntryLatched;
static UINT32 EntrySs0,EntrySs1;
BOOLEAN GuGetEntrySelectors(UINT32 *Ss0, UINT32 *Ss1) {
  if(!EntryLatched || !Ss0 || !Ss1)return FALSE;
  *Ss0=EntrySs0;*Ss1=EntrySs1;return TRUE;
}
BOOLEAN GuEntrySelectorsMatch(UINT32 Ss0, UINT32 Ss1) {
  return EntryLatched && Gu.prom_verified && !Gu.fault && Ss0==EntrySs0 && Ss1==EntrySs1;
}
BOOLEAN GuLatchEntrySelectors(UINT32 Ss0, UINT32 Ss1) {
  if(Gu.fault || !Gu.prom_verified || Gu.started || Ss0==0xffffffffU ||
     (Ss0&0xffff0000U)==0xbadf0000U || Ss0==0x88888888U || Ss1>0xfU)return FALSE;
  if(EntryLatched)return GuEntrySelectorsMatch(Ss0,Ss1);
  EntrySs0=Ss0;EntrySs1=Ss1;EntryLatched=TRUE;return TRUE;
}
#endif
#ifdef GU_ENTRY_LATCHED_SS1_FULL
static UINT32 SelectorPhase;
static BOOLEAN Ss1WriteConsumed;
BOOLEAN GuSelectorPhase(UINT32 Phase) {
  UINT32 entry0,entry1,ss0,ss1;
  /* ENTRY is established only by GuInit, never by this transition API. */
  if(!((SelectorPhase==GU_SELECTOR_PHASE_ENTRY && Phase==GU_SELECTOR_PHASE_SS0_WRITTEN && !Ss1WriteConsumed) ||
       (SelectorPhase==GU_SELECTOR_PHASE_SS0_WRITTEN && Phase==GU_SELECTOR_PHASE_SS1_WRITE && !Ss1WriteConsumed) ||
       (SelectorPhase==GU_SELECTOR_PHASE_SS1_WRITE && Phase==GU_SELECTOR_PHASE_TARGET && Ss1WriteConsumed))) {
    Gu.fault=TRUE;return FALSE;
  }
  if(!Gu.profile_verified || !Gu.armed || !Gu.owned || Gu.fault || !Gu.prom_verified ||
     !GuGetEntrySelectors(&entry0,&entry1))return FALSE;
  ss0=GuRead32(0x82381c);ss1=GuRead32(0x823820);
  if(Gu.fault || ss0!=0x88888888U ||
     (Phase==GU_SELECTOR_PHASE_SS0_WRITTEN && ss1!=entry1) ||
     (Phase==GU_SELECTOR_PHASE_SS1_WRITE && ss1!=entry1) ||
     (Phase==GU_SELECTOR_PHASE_TARGET && ss1!=0x8U)) {
    Gu.fault=TRUE;return FALSE;
  }
  (void)entry0;
  SelectorPhase=Phase;return TRUE;
}
#endif
VOID GuInit(EFI_BOOT_SERVICES *Bs, EFI_PCI_IO_PROTOCOL *Pci) {
  memset(&Gu,0,sizeof(Gu)); Gu.bs=Bs; Gu.pci=Pci;
#ifdef GU_ENTRY_LATCHED
  EntryLatched=FALSE;EntrySs0=EntrySs1=0;
#endif
#ifdef GU_ENTRY_LATCHED_SS1_FULL
  SelectorPhase=GU_SELECTOR_PHASE_ENTRY;Ss1WriteConsumed=FALSE;
#endif
}
BOOLEAN GuTargetConfig(const UINT32 *C, UINTN S, UINTN B, UINTN D, UINTN F) {
  return C && !S && B==1 && !D && !F && C[0]==GU_BOARD_ID &&
    C[11]==GU_BOARD_SUBSYSTEM && C[2]==0x030000a1U && !((C[3]>>16)&0x7f);
}
BOOLEAN GuSafeToReturn(VOID) {
  return !Gu.retained && !Gu.allocations && !Gu.dma_count && !Gu.master_attributes_pending &&
    (!Gu.started || (Gu.quiesced && !Gu.fault));
}
VOID *GuAlloc(size_t N) {
  VOID *Memory=NULL;
  if(!N || !Gu.bs || EFI_ERROR(Gu.bs->AllocatePool(EfiLoaderData,N,&Memory)))return NULL;
  memset(Memory,0,N);return Memory;
}
VOID GuFree(VOID *Memory) {
  /* Pool scratch is CPU-only; DMA uses GuDmaFree's separate lifetime guard. */
  if(Memory && !Gu.retained)Gu.bs->FreePool(Memory);
}
VOID GuStarted(VOID) {Gu.started=TRUE;Gu.quiesced=FALSE;}
VOID GuQuiesced(VOID) {if(!Gu.fault)Gu.quiesced=TRUE;}
EFI_STATUS GuDmaAllocate(UINTN N, VOID **C, UINT64 *D) {
  EFI_STATUS Status; GU_DMA *Entry; UINTN Mapped;
  if(!C || !D)return EFI_INVALID_PARAMETER;
  *C=NULL;*D=0;
  if(GU_PREFLIGHT_ONLY || !Gu.profile_verified || !Gu.armed || !Gu.display_ok || !N || N>(128U<<20) || Gu.fault || Gu.retained || !Gu.pci || !Gu.owned)return EFI_INVALID_PARAMETER;
  Entry=GuAlloc(sizeof(*Entry));if(!Entry)return EFI_OUT_OF_RESOURCES;
  Entry->pages=(N+4095)/4096;Entry->bytes=Entry->pages*4096;
  Status=Gu.pci->AllocateBuffer(Gu.pci,AllocateAnyPages,EfiBootServicesData,
    Entry->pages,&Entry->cpu,0);
  if(EFI_ERROR(Status) || !Entry->cpu) {GuFree(Entry);return EFI_ERROR(Status)?Status:EFI_OUT_OF_RESOURCES;}
  memset(Entry->cpu,0,Entry->bytes);Mapped=Entry->bytes;
  Status=Gu.pci->Map(Gu.pci,EfiPciIoOperationBusMasterCommonBuffer,Entry->cpu,
    &Mapped,&Entry->dma,&Entry->mapping);
  if(!EFI_ERROR(Status))Entry->mapped=TRUE;
  if(EFI_ERROR(Status) || Mapped!=Entry->bytes || !Entry->dma ||
     (Entry->dma&4095) || Entry->dma>0xffffffffULL || Entry->bytes-1>0xffffffffULL-Entry->dma) {
    if(!EFI_ERROR(Status))Status=EFI_UNSUPPORTED;
    if(Entry->mapped && EFI_ERROR(Gu.pci->Unmap(Gu.pci,Entry->mapping))) {
      Entry->next=Gu.allocations;Gu.allocations=Entry;Gu.retained=TRUE;return Status;
    }
    if(EFI_ERROR(Gu.pci->FreeBuffer(Gu.pci,Entry->pages,Entry->cpu))) {
      Entry->mapped=FALSE;Entry->next=Gu.allocations;Gu.allocations=Entry;Gu.retained=TRUE;return Status;
    }
    GuFree(Entry);return Status;
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
    if(EFI_ERROR(Status)) {Gu.retained=TRUE;return Status;}
    Entry->mapped=FALSE;
  }
  Status=Gu.pci->FreeBuffer(Gu.pci,Entry->pages,Entry->cpu);
  if(EFI_ERROR(Status)) {Gu.retained=TRUE;return Status;}
  *Link=Entry->next;GuFree(Entry);if(Gu.dma_count)Gu.dma_count--;return EFI_SUCCESS;
}
UINT32 GuRead32(UINT32 O) {
  UINT32 Value=0xffffffffU;
  if(Gu.fault)return Value;
  if(!Gu.pci || (O&3) || (UINT64)O+4>Gu.bar_size ||
     EFI_ERROR(Gu.pci->Mem.Read(Gu.pci,EfiPciIoWidthUint32,0,O,1,&Value))) {
    Gu.fault=TRUE;return 0xffffffffU;
  }
  return Value;
}
VOID GuWrite32(UINT32 O, UINT32 V) {
  if(Gu.fault)return;
#ifdef GU_ENTRY_LATCHED_SS1_PRESERVE
  if(O==0x823820){Gu.fault=TRUE;return;}
#endif
  if(GU_PREFLIGHT_ONLY || !Gu.profile_verified || !Gu.pci || !Gu.armed || !Gu.owned || !Gu.display_ok || (O&3) ||
      (UINT64)O+4>Gu.bar_size) {Gu.fault=TRUE;return;}
#ifdef GU_ENTRY_LATCHED_SS1_FULL
  if(O==0x823820 && (V!=0x8U || SelectorPhase!=GU_SELECTOR_PHASE_SS1_WRITE || Ss1WriteConsumed)) {
    Gu.fault=TRUE;return;
  }
  /* Spend before the external callback: reentry and errors cannot retry. */
  if(O==0x823820)Ss1WriteConsumed=TRUE;
#endif
  Gu.mmio_writes++;
  if(EFI_ERROR(Gu.pci->Mem.Write(Gu.pci,EfiPciIoWidthUint32,0,O,1,&V)))Gu.fault=TRUE;
}
static EFI_STATUS RawRead(EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 O, VOID *V) {
  if(!Gu.pci || W>EfiPciIoWidthUint32 || O>256-((UINT32)1<<W))return EFI_INVALID_PARAMETER;
  return Gu.pci->Pci.Read(Gu.pci,W,O,1,V);
}
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 O, VOID *V) {
  EFI_STATUS Status;
  if(Gu.fault)return EFI_DEVICE_ERROR;
  Status=RawRead(W,O,V);if(EFI_ERROR(Status))Gu.fault=TRUE;return Status;
}
EFI_STATUS GuConfigWrite(EFI_PCI_IO_PROTOCOL_WIDTH W, UINT32 O, VOID *V) {
  EFI_STATUS Status;
  if(Gu.fault)return EFI_DEVICE_ERROR;
  if(GU_PREFLIGHT_ONLY || !Gu.profile_verified || !Gu.pci || !Gu.owned || !Gu.armed || !Gu.display_ok || W>EfiPciIoWidthUint32 ||
     O>256-((UINT32)1<<W)) {Gu.fault=TRUE;return EFI_ACCESS_DENIED;}
  Status=Gu.pci->Pci.Write(Gu.pci,W,O,1,V);
  if(EFI_ERROR(Status))Gu.fault=TRUE;return Status;
}
VOID GuStall(UINTN Microseconds) {
  Gu.bs->Stall(Microseconds);Gu.ticks+=Microseconds;
}
EFI_STATUS GuEnableMaster(VOID) {
  UINT32 Id=0;UINT16 Command=0xffff,Now=0xffff;
  UINT64 Supported=0,Before=0,After=0,Decode;
  EFI_STATUS Status=EFI_ACCESS_DENIED;
  if(GU_PREFLIGHT_ONLY || !Gu.profile_verified || Gu.fault || Gu.retained || Gu.started || Gu.master_attributes_pending ||
     !Gu.pci || !Gu.pci->Attributes || !Gu.owned || !Gu.armed || !Gu.display_ok)goto fail;
  Status=RawRead(EfiPciIoWidthUint32,0,&Id);
  if(EFI_ERROR(Status) || Id!=GU_BOARD_ID)goto fail;
  Status=RawRead(EfiPciIoWidthUint16,4,&Command);
  if(EFI_ERROR(Status) || Command==0xffff || (Command&4) || !(Command&2))goto fail;
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationSupported,0,&Supported);
  if(EFI_ERROR(Status) || !(Supported&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))goto fail;
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Before);
  Decode=(Command&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(Command&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  if(EFI_ERROR(Status) || (Before&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER) ||
     (Before&(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY))!=Decode)goto fail;
  GuLog("Bus Master enable intent: command=%#x attributes=%#llx\n",
    Command,(unsigned long long)Before);
  if(Gu.fault){Status=EFI_DEVICE_ERROR;goto fail;}
  Gu.master_original_attributes=Before;
  Gu.master_original_command=Command;
  Gu.master_pci=Gu.pci;
#ifdef GU_ENTRY_LATCHED
  /* No logging/callback window between this final comparison and enabling BME. */
  UINT32 Ss0=GuRead32(0x82381c),Ss1=GuRead32(0x823820);
  if(!GuEntrySelectorsMatch(Ss0,Ss1)){Status=EFI_NOT_READY;goto fail;}
#endif
  /* Track before the call: an error may be returned after a hardware change. */
  Gu.master_attributes_pending=TRUE;
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationEnable,EFI_PCI_IO_ATTRIBUTE_BUS_MASTER,NULL);
  if(EFI_ERROR(Status))goto fail;
  Status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&After);
  if(EFI_ERROR(Status) || After!=(Before|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))goto fail;
  Status=RawRead(EfiPciIoWidthUint16,4,&Now);
  if(EFI_ERROR(Status) || Now!=(UINT16)(Command|4))goto fail;
  GuLog("Bus Master enabled: command=%#x attributes=%#llx matched=1\n",
    Now,(unsigned long long)After);
  if(Gu.fault){Status=EFI_DEVICE_ERROR;goto fail;}
  return EFI_SUCCESS;
fail:
  Gu.fault=TRUE;
  if(!EFI_ERROR(Status))Status=EFI_DEVICE_ERROR;
  GuLog("Bus Master enable failed: status=%#llx command=%#x readback=%#x managed=%u\n",
    (unsigned long long)Status,Command,Now,Gu.master_attributes_pending);
  return Status;
}
EFI_STATUS GuClearMaster(VOID) {
  UINT32 Id=0;UINT16 Command=0xffff,Now=0xffff;EFI_STATUS Status;
  EFI_STATUS AttributeStatus=EFI_SUCCESS;UINT64 Attributes=~0ULL;
  if(GU_PREFLIGHT_ONLY)return EFI_ACCESS_DENIED;
  /* Containment remains permitted after a latched I/O failure. */
  Status=RawRead(EfiPciIoWidthUint32,0,&Id);
  if(EFI_ERROR(Status) || Id!=GU_BOARD_ID)goto fail;
  if(Gu.master_attributes_pending) {
    if(Gu.pci!=Gu.master_pci)AttributeStatus=EFI_COMPROMISED_DATA;
    else if(!Gu.pci->Attributes)AttributeStatus=EFI_UNSUPPORTED;
    else {
      AttributeStatus=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationDisable,
        EFI_PCI_IO_ATTRIBUTE_BUS_MASTER,NULL);
      EFI_STATUS ReadStatus=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Attributes);
      if(!EFI_ERROR(AttributeStatus) && EFI_ERROR(ReadStatus))AttributeStatus=ReadStatus;
      if(!EFI_ERROR(AttributeStatus) && Attributes!=Gu.master_original_attributes)
        AttributeStatus=EFI_DEVICE_ERROR;
    }
    Gu.master_last_attributes=Attributes;
    if(EFI_ERROR(AttributeStatus) && !EFI_ERROR(Gu.master_attribute_status))
      Gu.master_attribute_status=AttributeStatus;
  }
  /* Always check/clear hardware even if the protocol call failed or lied. */
  Status=RawRead(EfiPciIoWidthUint16,4,&Command);
  if(EFI_ERROR(Status) || Command==0xffff)goto fail;
  if(Command&4) {
    Command&=(UINT16)~4U;
    Status=Gu.pci->Pci.Write(Gu.pci,EfiPciIoWidthUint16,4,1,&Command);
    if(EFI_ERROR(Status))goto fail;
  }
  Status=RawRead(EfiPciIoWidthUint16,4,&Now);
  if(EFI_ERROR(Status) || Now==0xffff || (Now&4))goto fail;
  /* Do not silently repair unrelated bits changed by the bus driver's API. */
  if(Gu.master_attributes_pending && Now!=Gu.master_original_command)goto fail;
  Gu.master_last_command=Now;
  if(EFI_ERROR(AttributeStatus)) {
    Gu.fault=TRUE;Gu.retained=TRUE;return AttributeStatus;
  }
  Gu.master_attributes_pending=FALSE;
  return EFI_SUCCESS;
fail:
  Gu.master_last_command=Now;
  if(!EFI_ERROR(Status))Status=EFI_DEVICE_ERROR;
  if(!EFI_ERROR(Gu.master_hardware_status))Gu.master_hardware_status=Status;
  Gu.fault=TRUE;Gu.retained=TRUE;return Status;
}
EFI_STATUS GuSavePci(VOID) {
  UINTN I;
  if(Gu.saved || Gu.fault || !Gu.owned || Gu.pcie_cap<0x40 || Gu.pcie_cap>0xd4)return EFI_NOT_READY;
  for(I=0;I<64;I++)
    if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)I*4,&Gu.saved_config[I])))return EFI_DEVICE_ERROR;
  if(Gu.saved_config[0]!=GU_BOARD_ID || Gu.saved_config[11]!=GU_BOARD_SUBSYSTEM ||
     (Gu.saved_config[1]&4))return EFI_ACCESS_DENIED;
  if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Gu.saved_devctl)))return EFI_DEVICE_ERROR;
  if(Gu.saved_devctl&0x8000)return EFI_NOT_READY;
  if(Gu.pcie_version>=2 && EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,
     Gu.pcie_cap+0x28,&Gu.saved_devctl2)))return EFI_DEVICE_ERROR;
  Gu.saved=TRUE;return EFI_SUCCESS;
}
EFI_STATUS GuFlr(VOID) {
  UINT32 Identity=0,Capability=0;UINT16 Command=0xffff,Pending=0xffff,Ctl=0;
  UINTN Wait;EFI_STATUS ResetStatus;
  if(GU_PREFLIGHT_ONLY || !Gu.profile_verified || !Gu.saved || Gu.flrs || Gu.fault || !Gu.owned || !Gu.armed || !Gu.display_ok)return EFI_NOT_READY;
  if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,0,&Identity)) || Identity!=GU_BOARD_ID ||
     EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,4,&Command)) || Command==0xffff ||
     (Command&4) || !(Command&2) ||
     EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,Gu.pcie_cap+4,&Capability)) ||
     !(Capability&(1U<<28)))return EFI_ACCESS_DENIED;
  for(Wait=0;Wait<=1000000;Wait+=1000) {
    if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,Gu.pcie_cap+10,&Pending)) || Pending==0xffff)return EFI_DEVICE_ERROR;
    if(!(Pending&0x20))break;
    if(Wait==1000000)return EFI_TIMEOUT;
    GuStall(1000);
  }
  if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Ctl)))return EFI_DEVICE_ERROR;
  if(Ctl&0x8000)return EFI_NOT_READY;
#ifdef GU_ENTRY_LATCHED
  UINT32 Ss0=GuRead32(0x82381c),Ss1=GuRead32(0x823820),Entry0,Entry1;
  if(!Gu.prom_verified || Gu.fault || !GuGetEntrySelectors(&Entry0,&Entry1) ||
     Ss0!=0x88888888U ||
#ifdef GU_ENTRY_LATCHED_SS1_PRESERVE
     Ss1!=Entry1
#else
     Ss1!=0x8U || SelectorPhase!=GU_SELECTOR_PHASE_TARGET
#endif
     )return EFI_NOT_READY;
#endif
  Ctl|=0x8000;Gu.flrs++;GuStarted();
  ResetStatus=GuConfigWrite(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Ctl);
  /* No access of any kind to this function for at least 100 ms. */
  GuStall(100000);
  if(EFI_ERROR(ResetStatus))return EFI_DEVICE_ERROR;
  for(Wait=0;Wait<=1000000;Wait+=1000) {
    EFI_STATUS Status=RawRead(EfiPciIoWidthUint32,0,&Identity);
    if(!EFI_ERROR(Status) && Identity==GU_BOARD_ID)return EFI_SUCCESS;
    if(!EFI_ERROR(Status) && Identity!=0xffffffffU && Identity!=0)break;
    if(Wait<1000000)GuStall(1000);
  }
  Gu.fault=TRUE;return EFI_DEVICE_ERROR;
}
EFI_STATUS GuRestorePci(VOID) {
  UINT16 Command=0,BytePair=0;UINT32 Value=0;UINT8 Line;INTN Offset;
  if(!Gu.saved || Gu.flrs!=1 || Gu.fault)return EFI_NOT_READY;
  if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,4,&Command)) || Command==0xffff || (Command&4))return EFI_DEVICE_ERROR;
  Command&=(UINT16)~7U;
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint16,4,&Command)) ||
     EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,4,&BytePair)) || (BytePair&7))return EFI_DEVICE_ERROR;
  /* Decode disabled; restore high BAR dwords before their low dwords. */
  for(Offset=0x24;Offset>=0x10;Offset-=4) {
    Value=Gu.saved_config[Offset/4];
    if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint32,(UINT32)Offset,&Value)))return EFI_DEVICE_ERROR;
  }
  Value=Gu.saved_config[0x30/4]&~1U; /* Never enable expansion ROM decoding. */
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint32,0x30,&Value)))return EFI_DEVICE_ERROR;
  BytePair=(UINT16)Gu.saved_config[3];
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint16,0x0c,&BytePair)))return EFI_DEVICE_ERROR;
  Line=(UINT8)Gu.saved_config[0x3c/4];
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint8,0x3c,&Line)))return EFI_DEVICE_ERROR;
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint16,Gu.pcie_cap+8,&Gu.saved_devctl)))return EFI_DEVICE_ERROR;
  if(Gu.pcie_version>=2 && EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint16,
     Gu.pcie_cap+0x28,&Gu.saved_devctl2)))return EFI_DEVICE_ERROR;
  Command=(UINT16)Gu.saved_config[1]&~4U;
  if(EFI_ERROR(GuConfigWrite(EfiPciIoWidthUint16,4,&Command)))return EFI_DEVICE_ERROR;
  if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint16,4,&BytePair)) || BytePair!=Command)return EFI_DEVICE_ERROR;
  for(Offset=0x10;Offset<=0x24;Offset+=4) {
    if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)Offset,&Value)) ||
       Value!=Gu.saved_config[Offset/4])return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
