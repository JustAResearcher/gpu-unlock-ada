#include "ownership.h"
#include "app_io.h"
#include "runtime.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include "sha256.h"
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,
  PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID,GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,
  BindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID,LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
typedef struct {EFI_GUID protocol;EFI_OPEN_PROTOCOL_INFORMATION_ENTRY entry;} OWNER_RECORD;
#define MAX_RECORDS 32
static BOOLEAN Passive(UINT32 A) {
  return A==EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL || A==EFI_OPEN_PROTOCOL_GET_PROTOCOL || A==EFI_OPEN_PROTOCOL_TEST_PROTOCOL;
}
static VOID LogProtocol(const char *Prefix,UINTN Index,const EFI_GUID *Guid) {
  GuLog("%s[%u] guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
    Prefix,(unsigned)Index,Guid->Data1,(unsigned)Guid->Data2,(unsigned)Guid->Data3,
    (unsigned)Guid->Data4[0],(unsigned)Guid->Data4[1],(unsigned)Guid->Data4[2],(unsigned)Guid->Data4[3],
    (unsigned)Guid->Data4[4],(unsigned)Guid->Data4[5],(unsigned)Guid->Data4[6],(unsigned)Guid->Data4[7]);
}
/* Caller holds TPL_NOTIFY through the snapshot and, for Claim, acquisition. */
static EFI_STATUS Snapshot(OWNER_RECORD *Records,UINTN *Count) {
  EFI_GUID **protocols=NULL;UINTN protocol_count=0;BOOLEAN pci=FALSE;*Count=0;
  if(!Gu.bs->ProtocolsPerHandle)return EFI_UNSUPPORTED;
  EFI_STATUS status=Gu.bs->ProtocolsPerHandle(Gu.handle,&protocols,&protocol_count);
  if(!EFI_ERROR(status) && (!protocols || !protocol_count || protocol_count>32))status=EFI_COMPROMISED_DATA;
  for(UINTN i=0;!EFI_ERROR(status) && i<protocol_count;i++) {
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN count=0;
    if(!protocols[i]){status=EFI_COMPROMISED_DATA;break;}
    if(!memcmp(protocols[i],&PciGuid,sizeof(PciGuid)))pci=TRUE;
    status=Gu.bs->OpenProtocolInformation(Gu.handle,protocols[i],&entries,&count);
    if(!EFI_ERROR(status) && (count>MAX_RECORDS-*Count || (count && !entries)))status=EFI_COMPROMISED_DATA;
    if(!EFI_ERROR(status))for(UINTN j=0;j<count;j++) {
      Records[*Count].protocol=*protocols[i];Records[*Count].entry=entries[j];(*Count)++;
    }
    if(entries && EFI_ERROR(Gu.bs->FreePool(entries)))status=EFI_DEVICE_ERROR;
  }
  if(protocols && EFI_ERROR(Gu.bs->FreePool(protocols)))status=EFI_DEVICE_ERROR;
  return !EFI_ERROR(status) && !pci?EFI_NOT_FOUND:status;
}
static VOID LogRecords(const OWNER_RECORD *Records,UINTN Count,EFI_STATUS Status) {
  GuLog("ownership: records=%zu exclusive=%u status=%#llx\n",(size_t)Count,Gu.owned,(unsigned long long)Status);
  for(UINTN i=0;i<Count;i++) {
    const EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&Records[i].entry;
    GuLog("owner[%u] protocol=%08x attributes=%#x count=%u agent=%#llx controller=%#llx\n",
      (unsigned)i,Records[i].protocol.Data1,e->Attributes,e->OpenCount,
      (unsigned long long)(UINTN)e->AgentHandle,(unsigned long long)(UINTN)e->ControllerHandle);
    LogProtocol("owner-protocol",i,&Records[i].protocol);
  }
}
EFI_STATUS GuClaimController(VOID) {
#if !GU_ENABLE_UNLOCK
  return EFI_ACCESS_DENIED;
#else
  OWNER_RECORD records[MAX_RECORDS];UINTN count=0;
  EFI_TPL old=Gu.bs->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS status=Snapshot(records,&count);
  if(!EFI_ERROR(status))for(UINTN i=0;i<count;i++)if(!Passive(records[i].entry.Attributes))status=EFI_ACCESS_DENIED;
  if(!EFI_ERROR(status)) {
    EFI_PCI_IO_PROTOCOL *pci=NULL;
    status=Gu.bs->OpenProtocol(Gu.handle,&PciGuid,(VOID **)&pci,Gu.image,NULL,EFI_OPEN_PROTOCOL_EXCLUSIVE);
    if(!EFI_ERROR(status)){Gu.owned=TRUE;if(pci!=Gu.pci)status=EFI_COMPROMISED_DATA;}
  }
  Gu.bs->RestoreTPL(old);LogRecords(records,count,status);return status;
#endif
}
static BOOLEAN ConsoleHandle(EFI_SYSTEM_TABLE *Table,EFI_HANDLE Handle) {
  return Handle && (Handle==Table->ConsoleOutHandle || Handle==Table->ConsoleInHandle || Handle==Table->StandardErrorHandle);
}
#if !GU_ENABLE_UNLOCK
/* Observational diagnostic metadata only. Never invoke any child interface,
 * connect/disconnect a driver, or use these results to authorize release. */
static VOID DetailFree(VOID *Buffer,const char *Kind) {
  if(Buffer) {
    EFI_STATUS status=Gu.bs->FreePool(Buffer);
    if(EFI_ERROR(status))GuLog("child-detail free=%s status=%#llx\n",Kind,(unsigned long long)status);
  }
}
static VOID LogChildPath(EFI_HANDLE Handle) {
  EFI_DEVICE_PATH_PROTOCOL *path=NULL;
  EFI_STATUS status=Gu.bs->HandleProtocol(Handle,&PathGuid,(VOID **)&path);
  GuLog("child-detail path_status=%#llx path_present=%u\n",(unsigned long long)status,(unsigned)(path!=NULL));
  if(EFI_ERROR(status) || !path)return;
  UINTN bytes=0;
  for(UINTN i=0;i<128 && bytes<=4092 && GuLogReady();i++) {
    EFI_DEVICE_PATH_PROTOCOL *node=(VOID *)((UINT8 *)path+bytes);
    UINTN length=node->Length[0]+((UINTN)node->Length[1]<<8);
    GuLog("child-path[%u] offset=%u type=%u subtype=%u bytes=%u\n",(unsigned)i,(unsigned)bytes,
      (unsigned)node->Type,(unsigned)node->SubType,(unsigned)length);
    if(length<4 || length>4096-bytes){GuLogText("child-detail path_invalid=length\n");return;}
    bytes+=length;
    if(node->Type==END_DEVICE_PATH_TYPE) {
      if(node->SubType!=END_ENTIRE_DEVICE_PATH_SUBTYPE || length!=4) {
        GuLogText("child-detail path_invalid=end-node\n");return;
      }
      static const char digits[]="0123456789abcdef";char hex[65];UINT8 digest[32];
      GuSha256(path,bytes,digest);
      for(UINTN j=0;j<32;j++){hex[2*j]=digits[digest[j]>>4];hex[2*j+1]=digits[digest[j]&15];}
      hex[64]=0;GuLog("child-detail path_bytes=%u path_sha256=%s\n",(unsigned)bytes,hex);return;
    }
  }
  GuLogText("child-detail path_invalid=bounded-node-limit\n");
}
static VOID LogChildDetails(EFI_SYSTEM_TABLE *Table,EFI_HANDLE Handle) {
  static EFI_GUID TextOutGuid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID,TextInGuid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
  GuLog("child-detail handle=%#llx console_out=%u console_in=%u stderr=%u observational_only=1\n",
    (unsigned long long)(UINTN)Handle,(unsigned)(Handle && Handle==Table->ConsoleOutHandle),
    (unsigned)(Handle && Handle==Table->ConsoleInHandle),(unsigned)(Handle && Handle==Table->StandardErrorHandle));
  if(!Handle || !GuLogReady())return;
  EFI_GUID **protocols=NULL;UINTN count=0,total=0;
  EFI_STATUS status=Gu.bs->ProtocolsPerHandle?Gu.bs->ProtocolsPerHandle(Handle,&protocols,&count):EFI_UNSUPPORTED;
  GuLog("child-detail protocol_status=%#llx protocol_count=%zu\n",(unsigned long long)status,(size_t)count);
  if(!EFI_ERROR(status) && protocols && count<=32)for(UINTN i=0;i<count && GuLogReady();i++) {
    if(!protocols[i]){GuLogText("child-detail protocol_invalid=null-guid\n");break;}
    LogProtocol("child-protocol",i,protocols[i]);
    const char *name=!memcmp(protocols[i],&PciGuid,sizeof(EFI_GUID))?"PCI_IO":
      !memcmp(protocols[i],&PathGuid,sizeof(EFI_GUID))?"DEVICE_PATH":
      !memcmp(protocols[i],&GopGuid,sizeof(EFI_GUID))?"GOP":
      !memcmp(protocols[i],&TextOutGuid,sizeof(EFI_GUID))?"SIMPLE_TEXT_OUT":
      !memcmp(protocols[i],&TextInGuid,sizeof(EFI_GUID))?"SIMPLE_TEXT_IN":"OTHER";
    GuLog("child-protocol[%u] name=%s\n",(unsigned)i,name);
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN entries_count=0;
    EFI_STATUS opened=Gu.bs->OpenProtocolInformation?
      Gu.bs->OpenProtocolInformation(Handle,protocols[i],&entries,&entries_count):EFI_UNSUPPORTED;
    GuLog("child-open protocol_index=%u status=%#llx records=%zu\n",(unsigned)i,(unsigned long long)opened,(size_t)entries_count);
    BOOLEAN bounded=entries_count<=32 && entries_count<=64-total && (!entries_count || entries);
    if(!EFI_ERROR(opened) && bounded) {
      for(UINTN j=0;j<entries_count && GuLogReady();j++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[j];
        GuLog("child-open[%u:%u] attributes=%#x count=%u agent=%#llx controller=%#llx\n",
          (unsigned)i,(unsigned)j,e->Attributes,e->OpenCount,(unsigned long long)(UINTN)e->AgentHandle,
          (unsigned long long)(UINTN)e->ControllerHandle);
      }
      total+=entries_count;
    } else if(!EFI_ERROR(opened))GuLogText("child-detail opens_invalid=bounded-record-limit-or-null\n");
    DetailFree(entries,"opens");
    if(!bounded || total>=64)break;
  } else if(!EFI_ERROR(status))GuLogText("child-detail protocols_invalid=bounded-count-or-null\n");
  DetailFree(protocols,"protocols");
  if(GuLogReady())LogChildPath(Handle);
}
#endif
static EFI_STATUS ChildRefused(const OWNER_RECORD *Record,BOOLEAN Log,const char *Reason,EFI_STATUS Status) {
  if(Log)GuLog("child-check handle=%#llx reason=%s status=%#llx\n",
    (unsigned long long)(UINTN)Record->entry.ControllerHandle,Reason,(unsigned long long)Status);
  return Status;
}
/* v0.4.1: permit only this driver's physical GOP children on the exact target.
 * A child record alone is insufficient: check GOP, geometry and PCI ancestry.
 * Hash its full bounded device path so final arming catches path replacement. */
static EFI_STATUS DisplayChild(const OWNER_RECORD *Record,GU_RELEASE_OWNER *Owner,BOOLEAN Log) {
  const EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&Record->entry;
  if(e->Attributes!=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER || e->AgentHandle!=Owner->agent ||
     e->OpenCount!=1 || !e->ControllerHandle || e->ControllerHandle==Gu.handle ||
     e->ControllerHandle==Gu.image || Owner->child_count>=GU_MAX_DISPLAY_CHILDREN ||
     (memcmp(&Record->protocol,&PciGuid,sizeof(PciGuid)) &&
      memcmp(&Record->protocol,&PathGuid,sizeof(PathGuid))))return ChildRefused(Record,Log,"record-policy",EFI_ACCESS_DENIED);
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=NULL;EFI_DEVICE_PATH_PROTOCOL *path=NULL;
  EFI_STATUS status=Gu.bs->HandleProtocol(e->ControllerHandle,&GopGuid,(VOID **)&gop);
  if(Log)GuLog("child-gop handle=%#llx status=%#llx present=%u mode_present=%u\n",
    (unsigned long long)(UINTN)e->ControllerHandle,(unsigned long long)status,(unsigned)(gop!=NULL),
    (unsigned)(!EFI_ERROR(status) && gop && gop->Mode));
  if(EFI_ERROR(status) || !gop)return ChildRefused(Record,Log,"gop-protocol",EFI_UNSUPPORTED);
  if(!gop->Mode)return ChildRefused(Record,Log,"gop-mode",EFI_UNSUPPORTED);
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *m=gop->Mode;
  if(Log)GuLog("child-gop mode_info=%u info_bytes=%zu framebuffer=%#llx framebuffer_bytes=%zu\n",
    (unsigned)(m->Info!=NULL),(size_t)m->SizeOfInfo,(unsigned long long)m->FrameBufferBase,(size_t)m->FrameBufferSize);
  if(Log && m->Info && m->SizeOfInfo>=sizeof(*m->Info))
    GuLog("child-gop width=%u height=%u pixel_format=%u pixels_per_scanline=%u\n",
      m->Info->HorizontalResolution,m->Info->VerticalResolution,(unsigned)m->Info->PixelFormat,m->Info->PixelsPerScanLine);
  if(!m->Info || m->SizeOfInfo<sizeof(*m->Info) || !m->FrameBufferBase || !m->FrameBufferSize ||
     !m->Info->HorizontalResolution || !m->Info->VerticalResolution ||
     m->Info->PixelFormat>PixelBlueGreenRedReserved8BitPerColor ||
     m->Info->PixelsPerScanLine<m->Info->HorizontalResolution ||
     (UINT64)m->Info->PixelsPerScanLine*m->Info->VerticalResolution>m->FrameBufferSize/4)
    return ChildRefused(Record,Log,"gop-geometry",EFI_UNSUPPORTED);
  status=Gu.bs->HandleProtocol(e->ControllerHandle,&PathGuid,(VOID **)&path);
  if(Log)GuLog("child-path lookup_status=%#llx path_present=%u locate_available=%u\n",
    (unsigned long long)status,(unsigned)(path!=NULL),(unsigned)(Gu.bs->LocateDevicePath!=NULL));
  if(EFI_ERROR(status) || !path || !Gu.bs->LocateDevicePath)return ChildRefused(Record,Log,"device-path",EFI_UNSUPPORTED);
  UINTN bytes=0;BOOLEAN end=FALSE;
  for(UINTN i=0;i<128 && bytes<=4092;i++) {
    EFI_DEVICE_PATH_PROTOCOL *node=(VOID *)((UINT8 *)path+bytes);
    UINTN length=node->Length[0]+((UINTN)node->Length[1]<<8);
    if(Log)GuLog("child-check-path[%u] offset=%u type=%u subtype=%u bytes=%u\n",(unsigned)i,(unsigned)bytes,
      (unsigned)node->Type,(unsigned)node->SubType,(unsigned)length);
    if(length<4 || length>4096-bytes)return ChildRefused(Record,Log,"path-length",EFI_COMPROMISED_DATA);
    bytes+=length;
    if(node->Type==END_DEVICE_PATH_TYPE) {
      if(node->SubType!=END_ENTIRE_DEVICE_PATH_SUBTYPE || length!=4)return ChildRefused(Record,Log,"path-end-node",EFI_UNSUPPORTED);
      end=TRUE;break;
    }
  }
  if(!end)return ChildRefused(Record,Log,"path-node-limit",EFI_COMPROMISED_DATA);
  EFI_DEVICE_PATH_PROTOCOL *remaining=path;EFI_HANDLE parent=NULL;
  status=Gu.bs->LocateDevicePath(&PciGuid,&remaining,&parent);
  if(Log)GuLog("child-parent locate_status=%#llx parent=%#llx target=%#llx\n",
    (unsigned long long)status,(unsigned long long)(UINTN)parent,(unsigned long long)(UINTN)Gu.handle);
  if(EFI_ERROR(status) || parent!=Gu.handle)return ChildRefused(Record,Log,"pci-parent",EFI_ACCESS_DENIED);
  for(UINTN i=0;i<Owner->child_count;i++)
    if(Owner->children[i].handle==e->ControllerHandle &&
       !memcmp(&Owner->children[i].protocol,&Record->protocol,sizeof(EFI_GUID)))
      return ChildRefused(Record,Log,"duplicate-child",EFI_COMPROMISED_DATA);
  GU_DISPLAY_CHILD *child=&Owner->children[Owner->child_count++];
  child->handle=e->ControllerHandle;child->protocol=Record->protocol;child->gop=gop;
  child->framebuffer=m->FrameBufferBase;child->framebuffer_size=m->FrameBufferSize;
  GuSha256(path,bytes,child->path_sha256);
  return EFI_SUCCESS;
}
EFI_STATUS GuFindReleaseOwner(EFI_SYSTEM_TABLE *Table,GU_RELEASE_OWNER *Owner,BOOLEAN Log) {
  OWNER_RECORD records[MAX_RECORDS];UINTN count=0,drivers=0;memset(Owner,0,sizeof(*Owner));
  if(!Gu.bs->DisconnectController || Gu.owned)return EFI_UNSUPPORTED;
  EFI_TPL old=Gu.bs->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS status=Snapshot(records,&count);
  Gu.bs->RestoreTPL(old);if(Log)LogRecords(records,count,status);
  if(EFI_ERROR(status))return status;
  for(UINTN i=0;i<count;i++) {
    const EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&records[i].entry;
    if(!memcmp(&records[i].protocol,&PciGuid,sizeof(PciGuid)) && e->Attributes==EFI_OPEN_PROTOCOL_BY_DRIVER) {
      if(!e->AgentHandle || e->ControllerHandle!=Gu.handle || e->OpenCount!=1)return EFI_ACCESS_DENIED;
      Owner->agent=e->AgentHandle;drivers++;
    }
  }
  if(drivers!=1 || Owner->agent==Gu.image || Owner->agent==Gu.handle || ConsoleHandle(Table,Owner->agent))return EFI_ACCESS_DENIED;
  for(UINTN i=0;i<count;i++) {
    const EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&records[i].entry;
    if(Passive(e->Attributes))continue;
    if(e->Attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) {
#if !GU_ENABLE_UNLOCK
      if(Log)LogChildDetails(Table,e->ControllerHandle);
#endif
      status=DisplayChild(&records[i],Owner,Log);
      if(Log)GuLog("display child: record=%u handle=%#llx agent=%#llx status=%#llx\n",
        (unsigned)i,(unsigned long long)(UINTN)e->ControllerHandle,
        (unsigned long long)(UINTN)e->AgentHandle,(unsigned long long)status);
      if(EFI_ERROR(status))return status;
      continue;
    }
    if(e->Attributes!=EFI_OPEN_PROTOCOL_BY_DRIVER || e->AgentHandle!=Owner->agent ||
       e->ControllerHandle!=Gu.handle || e->OpenCount!=1) {
      if(Log)GuLog("release refused: different owner or unsupported open at record %u\n",(unsigned)i);
      return EFI_ACCESS_DENIED;
    }
  }
  EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;EFI_LOADED_IMAGE_PROTOCOL *image=NULL;
  status=Gu.bs->HandleProtocol(Owner->agent,&BindingGuid,(VOID **)&binding);
  if(EFI_ERROR(status) || !binding || binding->DriverBindingHandle!=Owner->agent || !binding->Stop ||
    !binding->ImageHandle || binding->ImageHandle==Gu.image)return EFI_UNSUPPORTED;
  status=Gu.bs->HandleProtocol(binding->ImageHandle,&LoadedGuid,(VOID **)&image);
  if(EFI_ERROR(status) || !image)return EFI_UNSUPPORTED;
  if(Log)GuLog("release candidate: agent=%#llx image=%#llx source=%#llx base=%#llx bytes=%llu version=%#x code_type=%u children=%zu\n",
    (unsigned long long)(UINTN)Owner->agent,(unsigned long long)(UINTN)binding->ImageHandle,
    (unsigned long long)(UINTN)image->DeviceHandle,(unsigned long long)(UINTN)image->ImageBase,
    (unsigned long long)image->ImageSize,binding->Version,(unsigned)image->ImageCodeType,(size_t)Owner->child_count);
  if(!image->ImageBase || !image->ImageSize || image->ImageSize>64U*1024U*1024U || image->ImageCodeType!=EfiBootServicesCode)return EFI_UNSUPPORTED;
  Owner->image=binding->ImageHandle;Owner->base=image->ImageBase;Owner->size=image->ImageSize;
  Owner->version=binding->Version;Owner->stop=binding->Stop;return EFI_SUCCESS;
}
BOOLEAN GuSameReleaseOwner(const GU_RELEASE_OWNER *A,const GU_RELEASE_OWNER *B) {
  if(A->agent!=B->agent || A->image!=B->image || A->base!=B->base || A->size!=B->size ||
    A->version!=B->version || A->stop!=B->stop || A->child_count!=B->child_count)return FALSE;
  /* Snapshot iteration order is not an identity; compare complete record sets. */
  for(UINTN i=0;i<A->child_count;i++) {
    BOOLEAN match=FALSE;
    for(UINTN j=0;j<B->child_count;j++)
      if(!memcmp(&A->children[i],&B->children[j],sizeof(GU_DISPLAY_CHILD)))match=TRUE;
    if(!match)return FALSE;
  }
  return TRUE;
}
EFI_STATUS GuRefreshTargetPci(VOID) {
  EFI_PCI_IO_PROTOCOL *fresh=NULL;UINTN segment,bus,device,function;UINT32 config[64]={0};
  Gu.pci=NULL;
  if(!Gu.bs || !Gu.bs->HandleProtocol)return EFI_UNSUPPORTED;
  EFI_STATUS status=Gu.bs->HandleProtocol(Gu.handle,&PciGuid,(VOID **)&fresh);
  if(status!=EFI_SUCCESS)return EFI_ERROR(status)?status:EFI_DEVICE_ERROR;
  if(!fresh || !fresh->GetLocation || !fresh->Pci.Read || !fresh->Pci.Write)return EFI_NOT_FOUND;
  status=fresh->GetLocation(fresh,&segment,&bus,&device,&function);
  if(status!=EFI_SUCCESS)return EFI_ERROR(status)?status:EFI_DEVICE_ERROR;
  const UINT32 offsets[]={0,8,12,44};
  for(UINTN i=0;i<4;i++) {
    status=fresh->Pci.Read(fresh,EfiPciIoWidthUint32,offsets[i],1,&config[offsets[i]/4]);
    if(status!=EFI_SUCCESS)return EFI_ERROR(status)?status:EFI_DEVICE_ERROR;
  }
  if(!GuTargetConfig(config,segment,bus,device,function))return EFI_UNSUPPORTED;
  Gu.pci=fresh;return EFI_SUCCESS;
}
