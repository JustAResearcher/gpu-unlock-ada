/* SPDX-License-Identifier: GPL-2.0-only */
/* Reuse the synthetic EFI world, never captured firmware or the real GPU. */
#include <stdlib.h>
void *staged_calloc(size_t Count,size_t Bytes);
void staged_free(void *Pointer);
#define calloc staged_calloc
#define free staged_free
#define GuLog rehearsal_fixture_log
#define GuLogText rehearsal_fixture_log_text
#define GuTargetConfig rehearsal_fixture_target
#define main rehearsal_fixture_main
#include "test_rehearsal.c"
#undef main
#undef GuLogText
#undef GuLog
#undef GuTargetConfig
#undef free
#undef calloc

static const char *staged_case;
static char fixture_case[64];
static unsigned opens,closes,enables,post_checks,silent_post_checks;
static BOOLEAN exclusive_open,foreign_edge,missing_self,wrong_count,keyboard_drift,post_fail_late;
static GU_DMA outstanding_dma;
static BOOLEAN claim_phase;
static unsigned claim_notify_windows;
static BOOLEAN claim_window(void){return claim_phase&&claim_notify_windows==2&&current_tpl==TPL_NOTIFY;}
static struct {VOID *pointer;UINTN bytes;} mapped_pool[128];
static BOOLEAN refreshing;
static unsigned refresh_interfaces,refresh_reads,refresh_locations;
static EFI_PCI_IO_PROTOCOL alternate_pci;
/* All evidence tests use already-read synthetic data, never real PCI writes. */
static BOOLEAN evidence_active;
static unsigned evidence_maps,evidence_interfaces,evidence_locations,evidence_reads,evidence_attributes,evidence_bars;
static char evidence_log[131072];
static UINTN evidence_log_bytes;
static EFI_STATUS EFIAPI staged_bar(EFI_PCI_IO_PROTOCOL *,UINT8,UINT64 *,VOID **);
static EFI_STATUS EFIAPI staged_attributes(EFI_PCI_IO_PROTOCOL *,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION,UINT64,UINT64 *);
static EFI_STATUS EFIAPI staged_location(EFI_PCI_IO_PROTOCOL *,UINTN *,UINTN *,UINTN *,UINTN *);
static EFI_STATUS EFIAPI staged_read(EFI_PCI_IO_PROTOCOL *,EFI_PCI_IO_PROTOCOL_WIDTH,UINT32,UINTN,VOID *);

void *staged_calloc(size_t Count,size_t Bytes){
  VOID *p=calloc(Count,Bytes);if(!p)return NULL;
  for(UINTN i=0;i<128;i++)if(!mapped_pool[i].pointer){
    mapped_pool[i].pointer=p;mapped_pool[i].bytes=Count*Bytes;return p;
  }
  assert(!"synthetic pool tracking exhausted");return NULL;
}
void staged_free(void *Pointer){
  for(UINTN i=0;i<128;i++)if(mapped_pool[i].pointer==Pointer){
    mapped_pool[i].pointer=NULL;free(Pointer);return;
  }
  assert(!"freeing untracked synthetic pool");
}
VOID GuLog(const char *format,...){
  fixture_log_event(format);logs++;
  va_list ap;va_start(ap,format);
  if(evidence_active){
    va_list copy;va_copy(copy,ap);
    int n=vsnprintf(evidence_log+evidence_log_bytes,sizeof(evidence_log)-evidence_log_bytes,format,copy);
    va_end(copy);assert(n>=0&&(UINTN)n<sizeof(evidence_log)-evidence_log_bytes);evidence_log_bytes+=(UINTN)n;
  }
  vfprintf(stderr,format,ap);va_end(ap);
  if(evidence_active&&!strcmp(staged_case,"evidence-log-failure")&&strstr(format,"REHEARSAL_PCI_CHECK"))log_ok=FALSE;
}
VOID GuLogText(const char *text){GuLog("%s",text);}
BOOLEAN GuTargetConfig(const UINT32 *c,UINTN segment,UINTN bus,UINTN device,UINTN function){
  return rehearsal_fixture_target(c,segment,bus,device,function)&&
    (c[2]>>24)==3&&!((c[3]>>16)&0x7f);
}

static BOOLEAN si(const char *name){return !strcmp(staged_case,name);}
static BOOLEAN prefix(const char *name){return !strncmp(staged_case,name,strlen(name));}
static EFI_TPL EFIAPI staged_raise(EFI_TPL New){
  assert(current_tpl==TPL_APPLICATION&&New==TPL_NOTIFY);raises++;
  current_tpl=New;
  /* GuStagedClaim first verifies a coherent global snapshot, then enters its
   * distinct acquire window. Inject races only in that second raised window. */
  if(claim_phase)claim_notify_windows++;
  if(claim_window()&&si("claim-tpl-race-owner"))foreign_edge=TRUE;
  return TPL_APPLICATION;
}
static VOID EFIAPI staged_restore(EFI_TPL Old){
  assert(current_tpl==TPL_NOTIFY&&Old==TPL_APPLICATION);restores++;current_tpl=Old;
}
static EFI_STATUS EFIAPI staged_map(UINTN *bytes,EFI_MEMORY_DESCRIPTOR *map,
  UINTN *key,UINTN *stride,UINT32 *version){
  if(claim_window()&&si("claim-tpl-map-error"))return EFI_DEVICE_ERROR;
  if(refreshing&&si("refresh-map-error"))return EFI_DEVICE_ERROR;
  if(evidence_active)evidence_maps++;
  UINTN capacity=*bytes;
  EFI_STATUS s=memory_map(bytes,map,key,stride,version);if(s!=EFI_SUCCESS)return s;
  EFI_MEMORY_DESCRIPTOR all[160];UINTN n=*bytes/sizeof(*map);
  memcpy(all,map,*bytes);
  /* Model pool pages and static GUID storage so the new acquire-time scan can
   * validate EFI-returned arrays. Merge shared pages to avoid synthetic overlaps. */
  const VOID *guids[]={&pci_guid,&path_guid,&gop_guid,&text_guid,&input_guid,&binding_guid,
    &image_guid,&edid_guid,&unknown_guid,&console_device_tag_guid,&console_vendor_tag_guid,&root_guid,
    &usb_host_guid,&usb_io_guid,&block_io_guid};
  for(UINTN i=0;i<128+sizeof(guids)/sizeof(*guids);i++){
    const VOID *p=i<128?mapped_pool[i].pointer:guids[i-128];
    UINTN size=i<128?mapped_pool[i].bytes:sizeof(EFI_GUID);if(!p)continue;
    EFI_MEMORY_DESCRIPTOR d={0};d.Type=EfiBootServicesData;
    d.PhysicalStart=(UINTN)p&~(UINT64)4095;
    UINT64 end=((UINTN)p+(size?size:1)+4095)&~(UINT64)4095;
    d.NumberOfPages=(end-d.PhysicalStart)/4096;all[n++]=d;
  }
  if(!(refreshing&&si("refresh-code-unmapped"))){
    const VOID *callbacks[]={(VOID*)(UINTN)staged_location,(VOID*)(UINTN)staged_read,
      (VOID*)(UINTN)config_write,(VOID*)(UINTN)staged_attributes};
    for(UINTN i=0;i<sizeof(callbacks)/sizeof(*callbacks);i++){
      EFI_MEMORY_DESCRIPTOR d={0};d.Type=EfiBootServicesCode;
      d.PhysicalStart=(UINTN)callbacks[i]&~(UINT64)4095;d.NumberOfPages=1;
      if(refreshing&&(si("refresh-code-xp")||(si("refresh-late-code-xp")&&refresh_reads==4)))
        d.Attribute=EFI_MEMORY_XP;
      all[n++]=d;
    }
  }
  for(UINTN i=1;i<n;i++)for(UINTN j=i;j&&all[j].PhysicalStart<all[j-1].PhysicalStart;j--){
    EFI_MEMORY_DESCRIPTOR d=all[j];all[j]=all[j-1];all[j-1]=d;
  }
  UINTN used=0;
  for(UINTN i=0;i<n;i++){
    EFI_MEMORY_DESCRIPTOR *d=&all[i];
    if(used){
      EFI_MEMORY_DESCRIPTOR *prior=&all[used-1];
      UINT64 end=prior->PhysicalStart+prior->NumberOfPages*4096;
      if(prior->Type==d->Type&&prior->Attribute==d->Attribute&&d->PhysicalStart<=end){
        UINT64 next=d->PhysicalStart+d->NumberOfPages*4096;
        if(next>end)prior->NumberOfPages=(next-prior->PhysicalStart)/4096;
        continue;
      }
    }
    all[used++]=*d;
  }
  *bytes=used*sizeof(*map);if(*bytes>capacity)return EFI_BUFFER_TOO_SMALL;
  memcpy(map,all,*bytes);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI staged_protocols(EFI_HANDLE handle,EFI_GUID ***out,UINTN *count){
  EFI_STATUS s=protocols(handle,out,count);
  if(s!=EFI_SUCCESS||!claim_window()||handle!=GPU)return s;
  if(si("claim-tpl-protocol-warning"))return EFI_WARN_UNKNOWN_GLYPH;
  if(si("claim-tpl-null-protocol-array")){
    assert(free_pool(*out)==EFI_SUCCESS);*out=NULL;return s;
  }
  if(si("claim-tpl-protocol-count"))*count=65;
  if(si("claim-tpl-null-guid"))(*out)[0]=NULL;
  if(si("claim-tpl-unreadable-guid"))(*out)[0]=(VOID*)0x1000;
  if(si("claim-tpl-duplicate-guid"))(*out)[1]=(*out)[0];
  if(si("claim-tpl-no-pci"))(*out)[0]=&unknown_guid;
  return s;
}
static EFI_STATUS EFIAPI staged_interface(EFI_HANDLE handle,EFI_GUID *guid,VOID **out){
  if(evidence_active&&handle==GPU&&equal_guid(guid,&pci_guid)){
    evidence_interfaces++;
    if(si("evidence-interface-warning")){*out=(VOID*)0x1000;return EFI_WARN_UNKNOWN_GLYPH;}
    if(si("evidence-interface-error")){*out=(VOID*)0x1000;return EFI_DEVICE_ERROR;}
    if(si("evidence-interface-null")){*out=NULL;return EFI_SUCCESS;}
    if(si("evidence-interface-unreadable")){*out=(VOID*)0x1000;return EFI_SUCCESS;}
    if(si("evidence-interface-swap")){alternate_pci=f.pci;*out=&alternate_pci;return EFI_SUCCESS;}
  }
  if(refreshing&&handle==GPU&&equal_guid(guid,&pci_guid)){
    refresh_interfaces++;*out=NULL;
    if(si("refresh-interface-warning"))return EFI_WARN_UNKNOWN_GLYPH;
    if(si("refresh-interface-error"))return EFI_DEVICE_ERROR;
    if(si("refresh-interface-null"))return EFI_SUCCESS;
    if(si("refresh-interface-unreadable")){*out=(VOID*)0x1000;return EFI_SUCCESS;}
    if(si("refresh-interface-pointer")||(si("refresh-late-pointer")&&refresh_interfaces==2)){
      alternate_pci=f.pci;*out=&alternate_pci;return EFI_SUCCESS;
    }
  }
  return interface(handle,guid,out);
}
static EFI_STATUS EFIAPI staged_location(EFI_PCI_IO_PROTOCOL *io,UINTN *segment,UINTN *bus,UINTN *device,UINTN *function){
  if(evidence_active){
    evidence_locations++;
    if(si("evidence-location-warning"))return EFI_WARN_UNKNOWN_GLYPH;
    if(si("evidence-location-error"))return EFI_DEVICE_ERROR;
  }
  if(refreshing){
    refresh_locations++;
    if(si("refresh-location-warning"))return EFI_WARN_UNKNOWN_GLYPH;
    if(si("refresh-location-error"))return EFI_DEVICE_ERROR;
  }
  EFI_STATUS s=location(io,segment,bus,device,function);
  if(refreshing&&si("refresh-wrong-bus"))*bus=2;
  return s;
}
static EFI_STATUS EFIAPI staged_read(EFI_PCI_IO_PROTOCOL *io,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 offset,UINTN count,VOID *out){
  if(evidence_active){
    evidence_reads++;assert(width==EfiPciIoWidthUint32&&offset==0&&count==64);
    if(si("evidence-read-warning")||si("evidence-read-error")){
      memset(out,0xde,256);return si("evidence-read-warning")?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;
    }
  }
  if(refreshing){
    refresh_reads++;
    assert(width==EfiPciIoWidthUint32&&count==1&&(offset==0||offset==8||offset==12||offset==44));
    if(si("refresh-read-warning"))return EFI_WARN_UNKNOWN_GLYPH;
    if(si("refresh-read-error"))return EFI_DEVICE_ERROR;
  }
  EFI_STATUS s=config_read(io,width,offset,count,out);
  if(refreshing&&offset==44&&si("refresh-late-table"))f.pci.Pci.Write=NULL;
  return s;
}
static VOID master(BOOLEAN Enabled){
  if(Enabled){f.config[1]|=4;f.attributes|=EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;}
  else{f.config[1]&=~4u;f.attributes&=~EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;}
}
static EFI_STATUS EFIAPI staged_attributes(EFI_PCI_IO_PROTOCOL *io,
  EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 attrs,UINT64 *out){
  if(op==EfiPciIoAttributeOperationGet){
    if(evidence_active){
      evidence_attributes++;
      if(si("evidence-attributes-warning")||si("evidence-attributes-error")){
        *out=0xdedededededededeULL;
        return si("evidence-attributes-warning")?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;
      }
    }
    return attributes(io,op,attrs,out);
  }
  if(op==EfiPciIoAttributeOperationSupported){
    assert(io==&f.pci&&!attrs&&out);
    if(si("claim-supported-error"))return EFI_DEVICE_ERROR;
    if(si("claim-supported-warning"))return EFI_WARN_UNKNOWN_GLYPH;
    *out=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
    if(si("claim-supported-missing-bits"))*out=EFI_PCI_IO_ATTRIBUTE_MEMORY;
    if(si("claim-supported-mutation")){f.config[1]|=1;f.attributes|=EFI_PCI_IO_ATTRIBUTE_IO;}
    return EFI_SUCCESS;
  }
  assert(io==&f.pci&&op==EfiPciIoAttributeOperationEnable);
  UINT64 missing=(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY)&~f.attributes;
  assert(attrs&&attrs==missing);
  assert(stage==3&&exclusive_open&&Gu.owned);enables++;
  if(si("claim-attributes-error"))return EFI_DEVICE_ERROR;
  if(si("claim-attributes-warning"))return EFI_WARN_UNKNOWN_GLYPH;
  f.attributes|=attrs;
  if(attrs&EFI_PCI_IO_ATTRIBUTE_IO)f.config[1]|=1;
  if(attrs&EFI_PCI_IO_ATTRIBUTE_MEMORY)f.config[1]|=2;
  return si("claim-attributes-partial")?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI staged_bar(EFI_PCI_IO_PROTOCOL *io,UINT8 index,UINT64 *supports,VOID **resource){
  if(evidence_active)evidence_bars++;
  EFI_STATUS s=bar(io,index,supports,resource);
  if(evidence_active){
    if(si("evidence-bar-warning"))return EFI_WARN_UNKNOWN_GLYPH;
    if(si("evidence-bar-error"))return EFI_DEVICE_ERROR;
    if(si("evidence-bar-descriptor"))((EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR*)*resource)->AddrRangeMin+=0x1000000;
  }
  return s;
}
static EFI_STATUS EFIAPI staged_open(EFI_HANDLE handle,EFI_GUID *guid,VOID **out,
  EFI_HANDLE agent,EFI_HANDLE controller,UINT32 flags){
  assert(handle==GPU&&equal_guid(guid,&pci_guid)&&agent==APP&&controller==NULL);
  assert(flags==EFI_OPEN_PROTOCOL_EXCLUSIVE&&stage==3&&!exclusive_open);
  assert(current_tpl==TPL_NOTIFY);
  opens++;*out=NULL;
  if(si("claim-open-error"))return EFI_ACCESS_DENIED;
  if(si("claim-open-warning"))return EFI_WARN_UNKNOWN_GLYPH;
  exclusive_open=TRUE;
  *out=si("claim-wrong-interface")?(VOID*)&f.virtual_gop:&f.pci;
  return si("claim-open-partial")?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI staged_close(EFI_HANDLE handle,EFI_GUID *guid,
  EFI_HANDLE agent,EFI_HANDLE controller){
  assert(handle==GPU&&equal_guid(guid,&pci_guid)&&agent==APP&&controller==NULL);
  assert(stage==3&&exclusive_open);closes++;
  assert(current_tpl==TPL_APPLICATION);
  if(si("complete-close-error"))return EFI_DEVICE_ERROR;
  if(si("complete-close-warning"))return EFI_WARN_UNKNOWN_GLYPH;
  if(!si("complete-close-noop"))exclusive_open=FALSE;
  return si("complete-close-partial")?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI staged_information(EFI_HANDLE handle,EFI_GUID *guid,
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *count){
  EFI_STATUS status=information(handle,guid,out,count);
  if(status!=EFI_SUCCESS)return status;
  if(keyboard_drift&&handle==KEYBOARD&&equal_guid(guid,&input_guid)&&*count)
    (*out)[0].OpenCount++;
  BOOLEAN wrong_protocol=si("claim-edge-protocol");
  BOOLEAN own=exclusive_open&&!missing_self&&!si("claim-edge-missing")&&handle==GPU&&
    equal_guid(guid,wrong_protocol?&path_guid:&pci_guid);
  BOOLEAN extra=foreign_edge&&handle==GPU&&equal_guid(guid,&pci_guid);
  BOOLEAN passive=si("success-passive-edge")&&stage==3&&handle==GPU&&equal_guid(guid,&pci_guid);
  BOOLEAN zero_count=claim_window()&&si("claim-tpl-zero-open-count")&&handle==GPU&&equal_guid(guid,&pci_guid);
  UINTN add=(own?1:0)+(extra?1:0)+(passive?1:0)+(zero_count?1:0)+(own&&si("claim-edge-duplicate")?1:0);
  if(!add)return status;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=pool((*count+add)*sizeof(*entries));
  if(*count)memcpy(entries,*out,*count*sizeof(*entries));
  if(*out)assert(free_pool(*out)==EFI_SUCCESS);
  if(own){
    EFI_HANDLE agent=si("claim-edge-agent")?FOREIGN:APP;
    EFI_HANDLE controller=si("claim-edge-controller")?GPU:NULL;
    UINT32 flags=si("claim-edge-flags")?EFI_OPEN_PROTOCOL_BY_DRIVER:EFI_OPEN_PROTOCOL_EXCLUSIVE;
    UINT32 n=(wrong_count||si("claim-edge-count"))?2:1;
    entries[(*count)++]=ent(agent,controller,flags,n);
    if(si("claim-edge-duplicate"))entries[(*count)++]=ent(agent,controller,flags,n);
  }
  if(extra)entries[(*count)++]=ent(FOREIGN,GPU,EFI_OPEN_PROTOCOL_BY_DRIVER,1);
  if(passive)entries[(*count)++]=ent(FOREIGN,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL,1);
  if(zero_count)entries[(*count)++]=ent(FOREIGN,NULL,EFI_OPEN_PROTOCOL_GET_PROTOCOL,0);
  *out=entries;return EFI_SUCCESS;
}
EFI_STATUS GuStagedPostState(BOOLEAN Log){
  post_checks++;if(!Log)silent_post_checks++;
  if(Log&&usb_retired&&stage==3&&si("usb-poststate-extra-missing"))GuLog("SYNTHETIC_POSTSTATE_LOG\n");
  if(si("complete-poststate")&&stage==3)return EFI_NOT_READY;
  if(si("complete-postclose-state")&&closes)return EFI_NOT_READY;
  if(prefix("restore-poststate-")&&stage==(unsigned)atoi(strrchr(staged_case,'-')+1))
    return EFI_NOT_READY;
  if(post_fail_late)return EFI_NOT_READY;
  return EFI_SUCCESS;
}
static void synthetic_transaction_finished(void){
  Gu.started=TRUE;Gu.quiesced=TRUE;Gu.saved=TRUE;Gu.flrs=1;Gu.mmio_writes=9;
  Gu.dma_count=0;Gu.allocations=NULL;master(FALSE);
  /* A successful core restores the PCI config before releasing exclusivity. */
  f.config[1]|=3;f.attributes|=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
}
static void staged_setup(const char *name){
  staged_case=name;snprintf(fixture_case,sizeof(fixture_case),"deferred-success");
  if(prefix("release-error-"))snprintf(fixture_case,sizeof(fixture_case),"detach-error-%s",strrchr(name,'-')+1);
  if(prefix("release-partial-"))snprintf(fixture_case,sizeof(fixture_case),"detach-partial-%s",strrchr(name,'-')+1);
  if(prefix("restore-error-"))snprintf(fixture_case,sizeof(fixture_case),"connect-error-%s",strrchr(name,'-')+1);
  if(prefix("restore-partial-"))snprintf(fixture_case,sizeof(fixture_case),"connect-partial-%s",strrchr(name,'-')+1);
  if(prefix("restore-supported-"))snprintf(fixture_case,sizeof(fixture_case),"supported-error-%s",strrchr(name,'-')+1);
  if(prefix("usb-"))snprintf(fixture_case,sizeof(fixture_case),"%s",name);
  setup(fixture_case);
  f.bs.OpenProtocol=staged_open;f.bs.CloseProtocol=staged_close;
  f.bs.OpenProtocolInformation=staged_information;f.pci.Attributes=staged_attributes;
  f.bs.ProtocolsPerHandle=staged_protocols;f.bs.GetMemoryMap=staged_map;
  f.bs.RaiseTPL=staged_raise;f.bs.RestoreTPL=staged_restore;
  f.bs.HandleProtocol=staged_interface;f.pci.GetLocation=staged_location;f.pci.Pci.Read=staged_read;
  f.pci.GetBarAttributes=staged_bar;
  /* Staged post-FLR comparisons re-parse this CPU-owned capability chain. */
  f.config[1]|=0x00100000;f.config[0x34/4]=0x40;
  f.config[0x40/4]=0x00038001;f.config[0x80/4]=0x00020010;f.config[0x84/4]=1U<<28;
  Gu.pm_cap=0x40;Gu.pcie_cap=0x80;Gu.pcie_version=2;
  if(prefix("status-")){
    f.config[0x34/4]=0x60;f.config[0x40/4]=0;f.config[0x80/4]=0;f.config[0x84/4]=0;
    f.config[0x60/4]=0x00037801;f.config[0x78/4]=0x00020010;f.config[0x7c/4]=1U<<28;
    f.config[0x80/4]=0x00092930;Gu.pm_cap=0x60;Gu.pcie_cap=0x78;
    if(si("status-clear-correctable"))f.config[0x80/4]=0x00012930;
    if(si("status-clear-unsupported"))f.config[0x80/4]=0x00082930;
  }
  if(prefix("evidence-")){
    /* Status-shaped data outside the validated capability must remain exact. */
    f.config[0x78/4]=0x00090000;
  }
}
static void refresh_case(void){
  /* Containment must work even after a callback poisoned the graph or left
   * BME enabled. This API only authenticates the original PCI interface. */
  Gu.fault=TRUE;Gu.retained=TRUE;Gu.started=TRUE;Gu.saved=TRUE;
  Gu.mmio_writes=17;Gu.flrs=1;Gu.dma_count=1;Gu.allocations=&outstanding_dma;
  if(si("refresh-success-bme"))master(TRUE);
  if(si("refresh-success-graph-drift"))foreign_edge=TRUE;
  if(si("refresh-success-stale-pointer"))Gu.pci=(VOID*)0x1000;
  if(si("refresh-table-change"))f.pci.Pci.Write=NULL;
  if(si("refresh-id"))f.config[0]^=1;
  if(si("refresh-class"))f.config[2]=0x020000a1;
  if(si("refresh-header"))f.config[3]=0x10000;
  if(si("refresh-subsystem"))f.config[11]^=1;
  unsigned old_logs=logs,old_snapshots=snapshots,old_allocations=allocations;
  unsigned old_enables=enables,old_opens=opens,old_closes=closes,old_detaches=detaches,old_connects=connects;
  refreshing=TRUE;
  EFI_TPL old=staged_raise(TPL_NOTIFY);
  EFI_STATUS s=GuStagedRefreshPci();
  staged_restore(old);refreshing=FALSE;
  BOOLEAN success=prefix("refresh-success-");
  assert((s==EFI_SUCCESS)==success);
  assert(Gu.pci==(success?&f.pci:NULL));
  assert(Gu.fault&&Gu.retained&&Gu.started&&Gu.saved&&Gu.mmio_writes==17&&Gu.flrs==1&&
    Gu.dma_count==1&&Gu.allocations==&outstanding_dma);
  assert(logs==old_logs&&snapshots==old_snapshots&&allocations==old_allocations&&
    enables==old_enables&&opens==old_opens&&closes==old_closes&&detaches==old_detaches&&connects==old_connects);
  if(success)assert(refresh_interfaces==2&&refresh_locations==1&&refresh_reads==4);
  if(si("refresh-success-bme"))assert(f.config[1]&4);
}
static void stopped(void){
  unsigned old_connects=connects,old_detaches=detaches,old_opens=opens,old_closes=closes;
  assert(EFI_ERROR(GuStagedRestore()));assert(EFI_ERROR(GuRehearsalRestored()));
  assert(connects==old_connects&&detaches==old_detaches&&opens==old_opens&&closes==old_closes);
}
static void status_case(void){
  /* The reported pair is synthetic here; no captured config dump is embedded. */
  f.config[0x80/4]&=0xffff;
  if(si("status-partial-correctable"))f.config[0x80/4]|=0x10000;
  if(si("status-partial-unsupported"))f.config[0x80/4]|=0x80000;
  if(si("status-uncleared"))f.config[0x80/4]|=0x90000;
  if(si("status-new-nonfatal"))f.config[0x80/4]|=0x20000;
  if(si("status-new-fatal"))f.config[0x80/4]|=0x40000;
  if(si("status-pending"))f.config[0x80/4]|=0x200000;
  if(si("status-control"))f.config[0x80/4]^=1;
  if(si("status-other-dword"))f.config[4]^=0x1000000;
  if(si("status-stale-cap"))Gu.pcie_cap=0x80;
  if(si("status-stale-version"))Gu.pcie_version=1;
  if(si("status-malformed-cap"))f.config[0x60/4]=0x00036001;
  UINT32 before[64];memcpy(before,f.config,sizeof(before));
  unsigned old_opens=opens,old_enables=enables;
  evidence_active=TRUE;
  BOOLEAN accepted=prefix("status-clear-")||prefix("status-final-");
  EFI_STATUS result=GuStagedComplete();
  assert(result==(accepted?EFI_SUCCESS:EFI_NOT_READY));
  assert(!memcmp(before,f.config,sizeof(before))&&opens==old_opens&&enables==old_enables&&!forbidden);
  assert(evidence_maps==(accepted?15u:1u)&&evidence_interfaces==(accepted?3u:1u)&&
    evidence_locations==(accepted?3u:1u)&&evidence_reads==(accepted?3u:1u)&&
    evidence_attributes==(accepted?3u:1u)&&evidence_bars==(accepted?3u:0u));
  assert(Gu.started&&Gu.saved&&Gu.quiesced&&Gu.flrs==1&&Gu.mmio_writes==9&&!Gu.dma_count&&!Gu.allocations);
  if(!accepted){assert(!closes&&!connects);stopped();evidence_active=FALSE;return;}
  assert(closes==1&&GuStagedRestore()==EFI_SUCCESS&&connects==3);
  unsigned old_logs=logs;log_ok=FALSE;
  if(si("status-final-reassert"))f.config[0x80/4]|=0x90000;
  if(si("status-final-control"))f.config[0x80/4]^=1;
  if(si("status-final-flr-drift"))Gu.flrs=2;
  EFI_STATUS final=GuRehearsalRestored();
  assert(final==(prefix("status-final-")?EFI_NOT_READY:EFI_SUCCESS));
  assert(logs==old_logs&&closes==1&&connects==3&&!forbidden);
  evidence_active=FALSE;
}
static unsigned occurrences(const char *needle){
  unsigned count=0;for(const char *p=evidence_log;(p=strstr(p,needle));p+=strlen(needle))count++;
  return count;
}
static void evidence_case(void){
  if(si("evidence-status-only"))f.config[0x78/4]=0;
  if(si("evidence-control"))f.config[0x78/4]^=1;
  if(si("evidence-bar-config"))f.config[4]^=0x01000000;
  if(si("evidence-vendor"))f.config[0]^=1;
  if(si("evidence-subsystem"))f.config[11]^=1;
  if(si("evidence-cache"))f.config[3]^=0x10;
  if(si("evidence-raw-attributes"))f.attributes^=0x80000000;
  if(si("evidence-decode"))f.attributes&=~EFI_PCI_IO_ATTRIBUTE_MEMORY;
  if(si("evidence-bme"))f.config[1]|=4;
  if(si("evidence-interface-global")||si("evidence-interface-unreadable"))Gu.pci=(VOID*)0x1000;
  if(si("evidence-missing-callback"))f.pci.Pci.Read=NULL;
  if(si("evidence-table-change"))f.pci.RomSize=1;
  if(si("evidence-table-callback"))f.pci.Pci.Write=NULL;
  UINT32 expected_config[64];memcpy(expected_config,f.config,sizeof(expected_config));
  EFI_PCI_IO_PROTOCOL expected_api=f.pci;UINT64 expected_attrs=f.attributes;
  unsigned old_enables=enables,old_opens=opens,old_detaches=detaches;
  evidence_active=TRUE;
  EFI_STATUS s=GuStagedComplete();
  BOOLEAN success=si("evidence-success")||si("evidence-silent-final")||si("evidence-silent-final-refusal");
  assert((s==EFI_SUCCESS)==success);
  assert(!memcmp(expected_config,f.config,sizeof(expected_config))&&!memcmp(&expected_api,&f.pci,sizeof(f.pci))&&f.attributes==expected_attrs);
  assert(Gu.started&&Gu.saved&&Gu.quiesced&&Gu.flrs==1&&Gu.mmio_writes==9&&!Gu.dma_count&&!Gu.allocations);
  assert(enables==old_enables&&opens==old_opens&&detaches==old_detaches&&!connects&&!forbidden);
  if(success){
    /* Three validations each refresh their coherent snapshot map; diagnostic
     * evidence must add no GPU, BAR or attribute read. */
    assert(evidence_maps==15&&evidence_interfaces==3&&evidence_locations==3&&evidence_reads==3&&evidence_attributes==3&&evidence_bars==3);
    assert(closes==1&&!Gu.owned&&!exclusive_open);
    assert(GuStagedRestore()==EFI_SUCCESS&&stage==6&&connects==3);
    unsigned before=logs;
    if(si("evidence-silent-final")||si("evidence-silent-final-refusal"))log_ok=FALSE;
    if(si("evidence-silent-final-refusal"))f.config[0x78/4]=0;
    EFI_STATUS final=GuRehearsalRestored();
    assert(final==(si("evidence-silent-final-refusal")?EFI_NOT_READY:EFI_SUCCESS));
    if(!log_ok)assert(logs==before);
    assert(occurrences("REHEARSAL_PCI_SNAPSHOT source=baseline ")==16);
    assert(occurrences("REHEARSAL_PCI_SNAPSHOT source=current ")==16);
  }else{
    BOOLEAN interface_failure=prefix("evidence-interface-")||si("evidence-missing-callback")||prefix("evidence-table-");
    BOOLEAN location_failure=prefix("evidence-location-");
    BOOLEAN read_failure=prefix("evidence-read-");
    BOOLEAN identity_failure=si("evidence-vendor")||si("evidence-subsystem")||si("evidence-bme");
    BOOLEAN attrs_failure=prefix("evidence-attributes-");
    BOOLEAN before_bar=interface_failure||location_failure||read_failure||identity_failure||attrs_failure||
      si("evidence-status-only")||si("evidence-control")||si("evidence-bar-config")||si("evidence-cache")||
      si("evidence-raw-attributes")||si("evidence-decode");
    assert(evidence_maps==1&&evidence_interfaces==1&&evidence_locations==!interface_failure);
    assert(evidence_reads==!(interface_failure||location_failure));
    assert(evidence_attributes==!(interface_failure||location_failure||read_failure||identity_failure));
    assert(evidence_bars==!before_bar);
    assert(!closes&&Gu.owned&&exclusive_open);
    EFI_STATUS expected=EFI_NOT_READY;
    if(location_failure||read_failure||identity_failure||si("evidence-bar-descriptor"))expected=EFI_UNSUPPORTED;
    if(attrs_failure||si("evidence-bar-warning")||si("evidence-bar-error")||si("evidence-log-failure"))expected=EFI_DEVICE_ERROR;
    assert(s==expected);stopped();
  }
  evidence_active=FALSE;
}
int main(int argc,char **argv){
  assert(argc==2);staged_setup(argv[1]);
  if(si("refresh-premature")){refresh_case();goto done;}
  assert(EFI_ERROR(GuStagedRelease()));assert(EFI_ERROR(GuStagedClaim()));
  assert(EFI_ERROR(GuStagedComplete()));assert(EFI_ERROR(GuStagedRestore()));
  assert(!GuStagedDetached()&&EFI_ERROR(GuRehearsalRestored()));
  EFI_STATUS prepared=GuRehearsalPrepare(&f.table);
  if(replacement_case())assert(replacement_generation==(unsigned)(!replacement_stage4()&&!replacement_final()));
  if(replacement_refusal()){
    assert(EFI_ERROR(prepared)&&!detaches&&!connects&&!supported_calls&&!opens&&!closes&&
      !Gu.mmio_writes&&!Gu.flrs&&!Gu.started&&!Gu.owned&&!GuRehearsalMutated());goto done;
  }
  assert(prepared==EFI_SUCCESS);
  if(si("refresh-prepared-only")){refresh_case();goto done;}
  assert(EFI_ERROR(GuRehearsalRun()));
  assert(!detaches&&!connects&&!opens&&!closes&&!GuRehearsalMutated());
  Gu.armed=TRUE;
  EFI_STATUS status=GuStagedRelease();
  if(si("usb-replace-log-keyboard")){
    assert(EFI_ERROR(status)&&!detaches&&!connects&&!supported_calls&&!opens&&!closes&&
      !Gu.mmio_writes&&!Gu.flrs&&!Gu.started&&!GuRehearsalMutated());stopped();goto done;
  }
  if(prefix("release-")){
    assert(EFI_ERROR(status)&&!connects&&!opens&&!closes);
    assert(detaches==(unsigned)atoi(strrchr(staged_case,'-')+1));stopped();goto done;
  }
  assert(status==EFI_SUCCESS&&stage==3&&detaches==3&&!connects&&GuRehearsalMutated());
  if(prefix("refresh-")){refresh_case();goto done;}
  assert(EFI_ERROR(GuStagedRelease())&&detaches==3);
  assert(EFI_ERROR(GuStagedRestore())&&EFI_ERROR(GuRehearsalRestored()));
  if(si("status-preclaim-clear")){
    f.config[0x80/4]&=0xffff;assert(GuStagedClaim()==EFI_NOT_READY&&!opens&&!connects);stopped();goto done;
  }
  if(si("claim-preexisting-foreign"))foreign_edge=TRUE;
  if(si("claim-preexisting-self"))exclusive_open=TRUE;
  if(si("claim-preexisting-bme"))master(TRUE);
  if(si("success-no-decode-change")||si("success-memory-decode-missing")){
    f.config[1]|=1;f.attributes|=EFI_PCI_IO_ATTRIBUTE_IO;
  }
  if(si("success-no-decode-change")||si("success-io-decode-missing")){
    f.config[1]|=2;f.attributes|=EFI_PCI_IO_ATTRIBUTE_MEMORY;
  }
  claim_phase=TRUE;status=GuStagedClaim();claim_phase=FALSE;
  if(prefix("claim-")){
    assert(EFI_ERROR(status)&&!connects&&!closes);
    if(prefix("claim-preexisting-")||prefix("claim-tpl-"))assert(!opens);
    else assert(opens==1);
    stopped();goto done;
  }
  assert(status==EFI_SUCCESS&&Gu.owned&&exclusive_open&&opens==1);
  assert(enables==(si("success-no-decode-change")?0u:1u));
  assert(EFI_ERROR(GuStagedClaim())&&opens==1);
  assert(EFI_ERROR(GuStagedRestore())&&EFI_ERROR(GuRehearsalRestored()));
  if(si("status-preflr-clear")){
    f.config[0x80/4]&=0xffff;Gu.started=TRUE;Gu.saved=TRUE;Gu.mmio_writes=5;
    assert(!GuStagedDetached()&&!closes&&!connects);stopped();goto done;
  }
  if(prefix("detached-")){
    Gu.started=TRUE;Gu.saved=TRUE;Gu.mmio_writes=5;
    Gu.allocations=&outstanding_dma;Gu.dma_count=1;
    if(si("detached-bme"))master(TRUE);
    if(si("detached-flr"))Gu.flrs=1;
    if(si("detached-fault"))Gu.fault=TRUE;
    if(si("detached-master-pending"))Gu.master_attributes_pending=TRUE;
    if(si("detached-foreign"))foreign_edge=TRUE;
    assert(GuStagedDetached()==si("detached-live-dma"));
    assert(Gu.mmio_writes==5&&Gu.dma_count==1&&Gu.allocations==&outstanding_dma);
    stopped();goto done;
  }
  synthetic_transaction_finished();
  if(prefix("status-")){status_case();goto done;}
  if(prefix("evidence-")){evidence_case();goto done;}
  if(si("complete-not-started"))Gu.started=FALSE;
  if(si("complete-not-quiesced"))Gu.quiesced=FALSE;
  if(si("complete-not-saved"))Gu.saved=FALSE;
  if(si("complete-no-flr"))Gu.flrs=0;
  if(si("complete-extra-flr"))Gu.flrs=2;
  if(si("complete-no-writes"))Gu.mmio_writes=0;
  if(si("complete-dma-count"))Gu.dma_count=1;
  if(si("complete-dma-list"))Gu.allocations=&outstanding_dma;
  if(si("complete-fault"))Gu.fault=TRUE;
  if(si("complete-retained"))Gu.retained=TRUE;
  if(si("complete-master-pending"))Gu.master_attributes_pending=TRUE;
  if(si("complete-master-attribute-error"))Gu.master_attribute_status=EFI_DEVICE_ERROR;
  if(si("complete-master-hardware-error"))Gu.master_hardware_status=EFI_DEVICE_ERROR;
  if(si("complete-bme"))master(TRUE);
  if(si("complete-config"))f.config[4]^=0x1000000;
  if(si("complete-extra-edge"))foreign_edge=TRUE;
  if(si("complete-missing-self"))missing_self=TRUE;
  if(si("complete-count-drift"))wrong_count=TRUE;
  if(si("complete-keyboard-drift"))keyboard_drift=TRUE;
  UINT32 transaction_writes=Gu.mmio_writes,transaction_flrs=Gu.flrs;
  status=GuStagedComplete();
  assert(Gu.mmio_writes==transaction_writes&&Gu.flrs==transaction_flrs);
  if(prefix("complete-")){
    assert(EFI_ERROR(status)&&!connects);
    if(prefix("complete-close-")||si("complete-postclose-state"))assert(closes==1);
    else assert(!closes);
    stopped();goto done;
  }
  assert(status==EFI_SUCCESS&&!exclusive_open&&!Gu.owned&&closes==1);
  assert(Gu.started&&Gu.quiesced&&Gu.saved&&Gu.flrs==1&&Gu.mmio_writes==9&&!Gu.dma_count&&!Gu.allocations);
  assert(EFI_ERROR(GuStagedComplete())&&closes==1);
  assert(!GuStagedDetached()&&EFI_ERROR(GuRehearsalRestored()));
  if(si("restore-extra-edge"))foreign_edge=TRUE;
  if(si("restore-dma-count"))Gu.dma_count=1;
  status=GuStagedRestore();
  assert(Gu.mmio_writes==transaction_writes&&Gu.flrs==transaction_flrs);
  if(prefix("usb-")&&!usb_success()){
    assert(EFI_ERROR(status)&&stage==3&&!connects&&supported_calls==
      (unsigned)(si("usb-log-extra-missing")||si("usb-log-added-edge"))&&closes==1&&!Gu.owned&&!exclusive_open);
    assert(Gu.mmio_writes==9&&Gu.flrs==1);stopped();goto done;
  }
  if(prefix("restore-")){
    assert(EFI_ERROR(status));
    if(si("restore-extra-edge")||si("restore-dma-count"))assert(!connects);
    else if(prefix("restore-poststate-"))assert(connects==(unsigned)atoi(strrchr(staged_case,'-')+1)-3);
    else if(prefix("restore-supported-"))assert(connects==(unsigned)atoi(strrchr(staged_case,'-')+1)-1);
    else assert(connects==(unsigned)atoi(strrchr(staged_case,'-')+1));
    stopped();goto done;
  }
  assert(status==EFI_SUCCESS&&stage==6&&connects==3&&supported_calls==3);
  unsigned before_final_logs=logs;
  if(si("success-silent-final"))log_ok=FALSE;
  if(si("usb-retire-final")||si("usb-final-extra-missing")){usb_retired=TRUE;log_ok=FALSE;}
  if(replacement_final()||si("usb-replace-generation-two")){replacement_change();log_ok=FALSE;}
  assert((GuRehearsalRestored()==EFI_SUCCESS)==!(si("usb-final-extra-missing")||si("usb-replace-final-refuse")));
  if(si("success-silent-final")||si("usb-retire-final")||si("usb-final-extra-missing")||replacement_final()||si("usb-replace-generation-two"))assert(logs==before_final_logs&&silent_post_checks);
  assert(EFI_ERROR(GuStagedRestore())&&connects==3);
  assert(Gu.started&&Gu.quiesced&&Gu.saved&&Gu.flrs==1&&Gu.mmio_writes==9&&!Gu.owned);
  if(replacement_success())assert(replacement_notes==(replacement_final()?0U:
    si("usb-replace-preflight-partial")?3U:si("usb-replace-preflight-retirement")?5U:6U));
  if(si("late-poststate")){post_fail_late=TRUE;assert(EFI_ERROR(GuRehearsalRestored()));}
done:
  assert(!forbidden&&current_tpl==TPL_APPLICATION&&raises==restores);cleanup();
  for(UINTN i=0;i<128;i++)assert(!mapped_pool[i].pointer);
  printf("%s: staged adapter PASS detach=%u start=%u open=%u close=%u post_checks=%u\n",
    staged_case,detaches,connects,opens,closes,post_checks);
  return 0;
}
