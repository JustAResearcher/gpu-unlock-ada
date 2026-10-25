/* SPDX-License-Identifier: GPL-2.0-only */
/* Staged firmware-driver release/restoration. Snapshot/delta design follows
 * the upstream MSI ownership adapter, with exact identities independently
 * captured on this board and a protected EDID-only sibling. No driver code
 * patches or direct MMIO writes. The rehearsal cannot claim, change PCI decode,
 * reset, map DMA, or execute firmware. The separately compiled staged-unlock
 * adapter claims PCI ownership and restores only missing original decode bits.
 * A matching fingerprint identifies an observed driver, not trusted behavior. */
#include "rehearsal.h"
#include "app_io.h"
#include "capture_pe.h"
#include "runtime.h"
#include "sha256.h"
#include "config_compare.h"
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <IndustryStandard/Acpi10.h>
#if !((GU_REHEARSAL && !GU_ENABLE_UNLOCK && !GU_STAGED_UNLOCK) || \
      (!GU_REHEARSAL && GU_ENABLE_UNLOCK && GU_STAGED_UNLOCK))
#error Select exactly one isolated rehearsal or staged-unlock adapter
#endif
#if GU_STAGED_UNLOCK
#include "staged_state.h"
#endif
#include "rehearsal_profile.h"

#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
#define MAX_HANDLES 4096
#define MAX_EDGES 4096
#define MAP_BYTES (256U*1024U)
#define MAX_USB_LEAVES 64
#define USB_PATH_BYTES 256
typedef struct {EFI_HANDLE handle,agent,controller;EFI_GUID protocol;UINT32 attributes,count;} EDGE;
typedef struct {UINTN count,handles_count;EFI_HANDLE handles[MAX_HANDLES];EDGE edges[MAX_EDGES];} GRAPH;
typedef struct {
  UINTN edge,host_bytes,child_bytes;
  EDGE owner;
  VOID *host_interface,*child_interface;
  EFI_HANDLE replacement,pending_replacement,logged_replacement;
  UINT8 host_path[USB_PATH_BYTES],child_path[USB_PATH_BYTES];
  BOOLEAN eligible,allowed,pending,logged;
} USB_LEAF;
typedef struct {
  EDGE edge;
  UINTN index,path_bytes;
  UINT8 path[USB_PATH_BYTES];
  const char *reason;
  BOOLEAN usable;
} USB_NEW_LEAF;
typedef struct {
  EFI_SYSTEM_TABLE *table;EFI_PCI_IO_PROTOCOL *pci;EFI_PCI_IO_PROTOCOL pci_api;
  EFI_HANDLE target,original_gop,original_edid,gop,edid,console,agents[3],images[3],devices[3],parents[3];
  EFI_DRIVER_BINDING_PROTOCOL *bindings[3];
  EFI_LOADED_IMAGE_PROTOCOL *loaded[3];VOID *bases[3];
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out,*err;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *in;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL out_callbacks,err_callbacks;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL in_callbacks;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop_interface,gop_callbacks;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *stage4_gop,stage4_callbacks;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE stage4_mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION stage4_info;
  UINT32 config[64];UINT64 attributes;
  UINTN stage,map_bytes,stride;UINT8 map[MAP_BYTES];
  BOOLEAN prepared,mutated,ran,stage4_seen;
#if GU_STAGED_UNLOCK
  BOOLEAN release_done,claim_attempted,claimed,claim_done,complete_attempted;
  BOOLEAN complete_done,claim_closed,restore_attempted,restore_done,staged_failed;
  BOOLEAN post_config_logged;
#endif
} PLAN;
static PLAN P;
static GRAPH Baseline,Current;
static USB_LEAF UsbLeaves[MAX_USB_LEAVES];
static USB_NEW_LEAF UsbNewLeaves[MAX_USB_LEAVES];
static UINTN UsbLeafCount,UsbNewCount,UsbNewOmitted,UsbReplacementLogs;
static BOOLEAN UsbFrozen;
#if GU_STAGED_UNLOCK
static BOOLEAN TransactionClean(VOID);
#endif
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID,
  GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,TextGuid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID,
  InputGuid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID,BindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID,
  ImageGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID RootGuid={0x2f707ebb,0x4a1a,0x11d4,{0x9a,0x38,0,0x90,0x27,0x3f,0xc1,0x4d}};
static EFI_GUID UsbHcGuid={0x3e745226,0x9818,0x45b6,{0xa2,0xac,0xd7,0xcd,0x0e,0x8b,0xa2,0xbc}};
static EFI_GUID UsbIoGuid={0x2b2f68d6,0x0cd2,0x44cf,{0x8e,0x8b,0xbb,0xa2,0x0b,0x1b,0x5b,0x75}};
static VOID UsbObserve(const GRAPH *Graph);

static EFI_STATUS Result(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
static BOOLEAN Equal(const VOID *A,const VOID *B,UINTN N){return !memcmp(A,B,N);}
static BOOLEAN Guid(const EFI_GUID *A,const EFI_GUID *B){return Equal(A,B,sizeof(*A));}
static EFI_STATUS Free(VOID *Pointer,EFI_STATUS Status) {
  if(Pointer&&(Gu.bs->FreePool(Pointer)!=EFI_SUCCESS)&&Status==EFI_SUCCESS)return EFI_DEVICE_ERROR;
  return Result(Status);
}
static EFI_STATUS Map(VOID) {
  P.map_bytes=sizeof(P.map);UINTN key=0;UINT32 version=0;
  EFI_STATUS s=Gu.bs->GetMemoryMap(&P.map_bytes,(VOID *)P.map,&key,&P.stride,&version);
  if(s==EFI_SUCCESS&&(P.map_bytes>sizeof(P.map)||P.stride<sizeof(EFI_MEMORY_DESCRIPTOR)||
    P.stride>256||P.map_bytes%P.stride||P.map_bytes/P.stride>4096||version!=1))s=EFI_COMPROMISED_DATA;
  if(s!=EFI_SUCCESS){P.map_bytes=0;P.stride=0;}return Result(s);
}
static BOOLEAN Accessible(const VOID *Pointer,UINTN Bytes,BOOLEAN Execute) {
  UINT64 cursor=(UINTN)Pointer,end;
  if(!cursor||!Bytes||Bytes>~(UINT64)0-cursor||!P.map_bytes||!P.stride)return FALSE;
  end=cursor+Bytes;
  while(cursor<end) {
    UINT64 next=0;UINTN matches=0;
    for(UINTN i=0;i<P.map_bytes;i+=P.stride) {
      EFI_MEMORY_DESCRIPTOR d;memcpy(&d,P.map+i,sizeof(d));
      if(d.NumberOfPages>(~(UINT64)0-d.PhysicalStart)/4096)return FALSE;
      UINT64 limit=d.PhysicalStart+d.NumberOfPages*4096;
      if(cursor<d.PhysicalStart||cursor>=limit)continue;
      if(++matches!=1||(d.Attribute&EFI_MEMORY_RP)||
        (Execute&&((d.Attribute&EFI_MEMORY_XP)||
          (d.Type!=EfiLoaderCode&&d.Type!=EfiBootServicesCode&&d.Type!=EfiRuntimeServicesCode)))||
        (d.Type!=EfiLoaderCode&&d.Type!=EfiLoaderData&&d.Type!=EfiBootServicesCode&&
         d.Type!=EfiBootServicesData&&d.Type!=EfiRuntimeServicesCode&&d.Type!=EfiRuntimeServicesData))return FALSE;
      next=limit;
    }
    if(matches!=1||next<=cursor)return FALSE;
    /* Stop at every descriptor boundary so an overlap beginning inside the
     * current span cannot be skipped by jumping to its containing region end. */
    for(UINTN i=0;i<P.map_bytes;i+=P.stride) {
      EFI_MEMORY_DESCRIPTOR d;memcpy(&d,P.map+i,sizeof(d));
      if(d.NumberOfPages&&d.PhysicalStart>cursor&&d.PhysicalStart<next)next=d.PhysicalStart;
    }
    cursor=next;
  }
  return TRUE;
}
static BOOLEAN Readable(const VOID *Pointer,UINTN Bytes){return Accessible(Pointer,Bytes,FALSE);}
static BOOLEAN Callable(const VOID *Pointer,UINTN Bytes){return Accessible(Pointer,Bytes,TRUE);}
static EFI_STATUS Interface(EFI_HANDLE Handle,EFI_GUID *Protocol,VOID **Value) {
  *Value=NULL;EFI_STATUS s=Gu.bs->HandleProtocol(Handle,Protocol,Value);
  return s==EFI_SUCCESS&&!*Value?EFI_COMPROMISED_DATA:Result(s);
}
static BOOLEAN Path(EFI_HANDLE Handle,const UINT8 *Bytes,UINTN Size) {
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  return Interface(Handle,&PathGuid,(VOID **)&p)==EFI_SUCCESS&&Readable(p,Size)&&Equal(p,Bytes,Size);
}
static VOID NoteInventory(EFI_HANDLE Handle,UINTN Expected,EFI_GUID **Protocols,UINTN Count,EFI_STATUS Status) {
  if(!GuLogReady())return;
  EFI_GUID copied[64];BOOLEAN readable[64]={0};
  BOOLEAN array_readable=Count&&Count<=COUNT(copied)&&Readable(Protocols,Count*sizeof(*Protocols));
  if(array_readable)for(UINTN i=0;i<Count;i++) {
    readable[i]=Readable(Protocols[i],sizeof(copied[i]));
    if(readable[i])memcpy(&copied[i],Protocols[i],sizeof(copied[i]));
  }
  GuLog("REHEARSAL_INVENTORY_REFUSED stage=%zu handle=%#llx expected=%zu actual=%zu status=%#llx array_readable=%u\n",
    (size_t)P.stage,(unsigned long long)(UINTN)Handle,(size_t)Expected,(size_t)Count,
    (unsigned long long)Status,(unsigned)array_readable);
  /* The returned array may itself be invalid. Diagnose only its bounded,
   * mapped entries, copying before logging and issuing no extra EFI query. */
  if(array_readable)for(UINTN i=0;i<Count;i++) {
    if(!readable[i]) {
      GuLog("REHEARSAL_INVENTORY_PROTOCOL index=%zu readable=0\n",(size_t)i);continue;
    }
    EFI_GUID *g=&copied[i];
    GuLog("REHEARSAL_INVENTORY_PROTOCOL index=%zu readable=1 guid=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
      (size_t)i,(unsigned)g->Data1,(unsigned)g->Data2,(unsigned)g->Data3,
      (unsigned)g->Data4[0],(unsigned)g->Data4[1],(unsigned)g->Data4[2],(unsigned)g->Data4[3],
      (unsigned)g->Data4[4],(unsigned)g->Data4[5],(unsigned)g->Data4[6],(unsigned)g->Data4[7]);
  }
}
static EFI_STATUS Inventory(EFI_HANDLE Handle,const EFI_GUID *Expected,UINTN Count) {
  EFI_GUID **protocols=NULL;UINTN n=0;
  EFI_STATUS s=Gu.bs->ProtocolsPerHandle(Handle,&protocols,&n);
  if(s==EFI_SUCCESS&&(!protocols||n!=Count||n>64))s=EFI_UNSUPPORTED;
  for(UINTN i=0;s==EFI_SUCCESS&&i<n;i++) {
    if(!protocols[i]){s=EFI_COMPROMISED_DATA;break;}
    UINTN matches=0;
    for(UINTN j=0;j<n;j++)if(Guid(protocols[i],&Expected[j]))matches++;
    if(matches!=1)s=EFI_UNSUPPORTED;
    for(UINTN j=0;j<i;j++)if(Guid(protocols[j],protocols[i]))s=EFI_COMPROMISED_DATA;
  }
  if(s!=EFI_SUCCESS)NoteInventory(Handle,Count,protocols,n,s);
  return Free(protocols,s);
}
static BOOLEAN SameEdge(const EDGE *A,const EDGE *B) {
  return A->handle==B->handle&&A->agent==B->agent&&A->controller==B->controller&&
    A->attributes==B->attributes&&A->count==B->count&&Guid(&A->protocol,&B->protocol);
}
static BOOLEAN ClaimEdge(const EDGE *Edge) {
#if GU_STAGED_UNLOCK
  return P.claimed&&Gu.owned&&P.stage==3&&Edge->handle==P.target&&Edge->agent==Gu.image&&
    !Edge->controller&&Guid(&Edge->protocol,&PciGuid)&&
    Edge->attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&Edge->count==1;
#else
  (void)Edge;return FALSE;
#endif
}
/* Caller holds TPL_NOTIFY. Only bounded memory/protocol-handler services are
 * used here; no log, filesystem, driver callback, stall or wait is permitted. */
static EFI_STATUS SnapshotLocked(GRAPH *Graph) {
  EFI_HANDLE *handles=NULL;UINTN count=0,protocol_total=0,open_total=0;
  memset(Graph,0,sizeof(*Graph));
  EFI_STATUS s=Gu.bs->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&handles);
  if(s==EFI_SUCCESS&&(!handles||!count||count>MAX_HANDLES))s=EFI_BAD_BUFFER_SIZE;
  if(s==EFI_SUCCESS){Graph->handles_count=count;memcpy(Graph->handles,handles,count*sizeof(*handles));}
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GUID **protocols=NULL;UINTN n=0;
    if(!handles[i]){s=EFI_COMPROMISED_DATA;break;}
    for(UINTN j=0;j<i;j++)if(handles[j]==handles[i])s=EFI_COMPROMISED_DATA;
    if(s!=EFI_SUCCESS)break;
    s=Gu.bs->ProtocolsPerHandle(handles[i],&protocols,&n);
    if(s==EFI_SUCCESS&&(!protocols||!n||n>64||protocol_total+n>16384))s=EFI_BAD_BUFFER_SIZE;
    protocol_total+=n;
    for(UINTN j=0;s==EFI_SUCCESS&&j<n;j++) {
      EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN m=0;
      if(!protocols[j]){s=EFI_COMPROMISED_DATA;break;}
      for(UINTN k=0;k<j;k++)if(Guid(protocols[k],protocols[j]))s=EFI_COMPROMISED_DATA;
      if(s!=EFI_SUCCESS)break;
      s=Gu.bs->OpenProtocolInformation(handles[i],protocols[j],&entries,&m);
      if(s==EFI_SUCCESS&&(m>128||(m&&!entries)||open_total+m>131072))s=EFI_BAD_BUFFER_SIZE;
      open_total+=m;
      for(UINTN k=0;s==EFI_SUCCESS&&k<m;k++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[k];
        if(!e->OpenCount){s=EFI_COMPROMISED_DATA;break;}
        if(e->Attributes==1||e->Attributes==2||e->Attributes==4)continue;
        if(Graph->count==MAX_EDGES){s=EFI_BAD_BUFFER_SIZE;break;}
        if(!e->AgentHandle){s=EFI_COMPROMISED_DATA;break;}
        EDGE edge={handles[i],e->AgentHandle,e->ControllerHandle,*protocols[j],e->Attributes,e->OpenCount};
        if(!e->ControllerHandle&&!ClaimEdge(&edge)){s=EFI_COMPROMISED_DATA;break;}
        for(UINTN z=0;z<Graph->count;z++)if(SameEdge(&edge,&Graph->edges[z]))s=EFI_COMPROMISED_DATA;
        if(s==EFI_SUCCESS)Graph->edges[Graph->count++]=edge;
      }
      s=Free(entries,s);
    }
    s=Free(protocols,s);
  }
  return Free(handles,s);
}
static EFI_STATUS Snapshot(GRAPH *Graph) {
  EFI_TPL previous=Gu.bs->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS s=Map();
  if(s==EFI_SUCCESS)s=SnapshotLocked(Graph);
  if(s==EFI_SUCCESS&&UsbFrozen&&Graph==&Current)UsbObserve(Graph);
  Gu.bs->RestoreTPL(previous);
  return Result(s);
}
static BOOLEAN HandleIn(const GRAPH *Graph,EFI_HANDLE Handle) {
  for(UINTN i=0;i<Graph->handles_count;i++)if(Graph->handles[i]==Handle)return TRUE;return FALSE;
}
static BOOLEAN UsbProtected(EFI_HANDLE Handle) {
  if(!Handle)return TRUE;
  if(Handle==Gu.image||Handle==P.target||Handle==P.original_gop||Handle==P.original_edid||
    Handle==P.gop||Handle==P.edid||Handle==P.console)return TRUE;
  for(UINTN i=0;i<3;i++)if(Handle==P.agents[i]||Handle==P.images[i]||
    Handle==P.devices[i]||Handle==P.parents[i])return TRUE;
  return FALSE;
}
static UINTN EdgeMatches(const GRAPH *Graph,const EDGE *Edge) {
  UINTN count=0;for(UINTN i=0;i<Graph->count;i++)if(SameEdge(&Graph->edges[i],Edge))count++;return count;
}
static BOOLEAN UsbUnused(const GRAPH *Graph,EFI_HANDLE Child,UINTN Ignore) {
  for(UINTN i=0;i<Graph->count;i++) {
    if(i==Ignore)continue;const EDGE *e=&Graph->edges[i];
    if(e->handle==Child||e->agent==Child||e->controller==Child)return FALSE;
  }
  return TRUE;
}
static BOOLEAN UsbPathCopy(EFI_HANDLE Handle,UINT8 *Bytes,UINTN *Size) {
  EFI_DEVICE_PATH_PROTOCOL *path=NULL;*Size=0;
  if(Interface(Handle,&PathGuid,(VOID **)&path)!=EFI_SUCCESS)return FALSE;
  UINTN at=0;
  while(at+4<=USB_PATH_BYTES) {
    UINT8 *p=(UINT8 *)path+at;
    if(!Readable(p,4))return FALSE;
    UINTN n=(UINTN)p[2]|((UINTN)p[3]<<8);
    if(n<4||n>USB_PATH_BYTES-at||!Readable(p,n))return FALSE;
    memcpy(Bytes+at,p,n);at+=n;
    if(p[0]==END_DEVICE_PATH_TYPE) {
      if(p[1]!=END_ENTIRE_DEVICE_PATH_SUBTYPE||n!=4)return FALSE;
      *Size=at;return TRUE;
    }
  }
  return FALSE;
}
static BOOLEAN UsbPaths(const USB_LEAF *Leaf) {
  /* The host is a PCI controller under a single ACPI root, never a display,
   * media path, or USB descendant. Only ordinary USB messaging nodes may be
   * appended to that exact host prefix for an eligible unbound interface. */
  if(Leaf->host_bytes<22||Leaf->host_path[0]!=ACPI_DEVICE_PATH||
    Leaf->host_path[1]!=ACPI_DP||Leaf->host_path[2]!=12||Leaf->host_path[3])return FALSE;
  UINTN at=12,pci=0;
  while(at+4<Leaf->host_bytes) {
    const UINT8 *p=Leaf->host_path+at;
    if(Leaf->host_bytes-at<10||p[0]!=HARDWARE_DEVICE_PATH||p[1]!=HW_PCI_DP||p[2]!=6||p[3])return FALSE;
    at+=6;if(++pci>16)return FALSE;
  }
  if(!pci||at+4!=Leaf->host_bytes||Leaf->child_bytes<at+10||
    !Equal(Leaf->host_path,Leaf->child_path,at))return FALSE;
  UINTN usb=0;
  while(at+4<Leaf->child_bytes) {
    const UINT8 *p=Leaf->child_path+at;
    if(Leaf->child_bytes-at<10||p[0]!=MESSAGING_DEVICE_PATH||p[1]!=MSG_USB_DP||p[2]!=6||p[3])return FALSE;
    at+=6;if(++usb>8)return FALSE;
  }
  return usb&&at+4==Leaf->child_bytes;
}
static BOOLEAN UsbBareInterface(EFI_HANDLE Child,VOID **InterfaceValue) {
  EFI_GUID **protocols=NULL;UINTN count=0,path=0,usb=0;
  EFI_STATUS s=Gu.bs->ProtocolsPerHandle(Child,&protocols,&count);
  if(s==EFI_SUCCESS&&(!protocols||count!=2))s=EFI_UNSUPPORTED;
  /* Protocol services may allocate a new pool page. Refresh after allocation
   * rather than validating their returned array against a stale map. */
  if(s==EFI_SUCCESS)s=Map();
  if(s==EFI_SUCCESS&&!Readable(protocols,count*sizeof(*protocols)))s=EFI_COMPROMISED_DATA;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    if(!Readable(protocols[i],sizeof(*protocols[i]))){s=EFI_COMPROMISED_DATA;break;}
    if(Guid(protocols[i],&PathGuid))path++;
    else if(Guid(protocols[i],&UsbIoGuid))usb++;
    else s=EFI_UNSUPPORTED;
  }
  s=Free(protocols,s);
  if(s!=EFI_SUCCESS||path!=1||usb!=1)return FALSE;
  return Interface(Child,&UsbIoGuid,InterfaceValue)==EFI_SUCCESS&&Readable(*InterfaceValue,1);
}
/* Protect every loaded image's source volume and parent, including our USB
 * volume and the preloaded Windows image. No new getters or filesystem calls
 * are needed. Unknown/broken image metadata disables this optional exception. */
static BOOLEAN UsbImageUses(const GRAPH *Graph,BOOLEAN *Blocked) {
  BOOLEAN self=FALSE;
  for(UINTN i=0;i<Graph->handles_count;i++) {
    EFI_LOADED_IMAGE_PROTOCOL *image=NULL;
    EFI_STATUS s=Interface(Graph->handles[i],&ImageGuid,(VOID **)&image);
    if(s==EFI_UNSUPPORTED||s==EFI_NOT_FOUND)continue;
    if(s!=EFI_SUCCESS||!Readable(image,sizeof(*image)))return FALSE;
    if(Graph->handles[i]==Gu.image)self=TRUE;
    for(UINTN n=0;n<UsbLeafCount;n++) {
      USB_LEAF *leaf=&UsbLeaves[n];EFI_HANDLE child=Baseline.edges[leaf->edge].controller;
      if(child==Graph->handles[i]||child==image->DeviceHandle||child==image->ParentHandle||
        (leaf->replacement&&(leaf->replacement==Graph->handles[i]||
          leaf->replacement==image->DeviceHandle||leaf->replacement==image->ParentHandle)))Blocked[n]=TRUE;
    }
  }
  return self;
}
static BOOLEAN UsbPathUses(const GRAPH *Graph,BOOLEAN *Blocked) {
  /* USB hubs can be graph leaves too: descendant devices need not open a
   * protocol on the hub handle. Therefore also preserve every path ancestor
   * and same-path alias, including passive input/storage/source volumes. */
  for(UINTN i=0;i<Graph->handles_count;i++) {
    EFI_DEVICE_PATH_PROTOCOL *path=NULL;
    EFI_STATUS s=Interface(Graph->handles[i],&PathGuid,(VOID **)&path);
    if(s==EFI_UNSUPPORTED||s==EFI_NOT_FOUND)continue;
    if(s!=EFI_SUCCESS)return FALSE;
    UINTN at=0,instance=0;BOOLEAN ended=FALSE;
    while(at+4<=4096) {
      const UINT8 *p=(const UINT8 *)path+at;
      if(!Readable(p,4))return FALSE;
      UINTN n=(UINTN)p[2]|((UINTN)p[3]<<8);
      if(n<4||n>4096-at||!Readable(p,n))return FALSE;at+=n;
      if(p[0]==END_DEVICE_PATH_TYPE) {
        if(n!=4||(p[1]!=END_ENTIRE_DEVICE_PATH_SUBTYPE&&p[1]!=END_INSTANCE_DEVICE_PATH_SUBTYPE))return FALSE;
        for(UINTN z=0;z<UsbLeafCount;z++) {
          USB_LEAF *leaf=&UsbLeaves[z];EFI_HANDLE child=Baseline.edges[leaf->edge].controller;
          if(Graph->handles[i]!=child&&Graph->handles[i]!=leaf->replacement&&at-instance>=leaf->child_bytes&&
            Equal((UINT8 *)path+instance,leaf->child_path,leaf->child_bytes-4))Blocked[z]=TRUE;
        }
        if(p[1]==END_ENTIRE_DEVICE_PATH_SUBTYPE){ended=TRUE;break;}
        instance=at;
      }
    }
    if(!ended)return FALSE;
  }
  return TRUE;
}
static VOID UsbNewObserve(const GRAPH *Graph) {
  /* Retain bounded metadata for previously unknown USB child relationships.
   * This does not authorize additions: only a unique exact-path match to a
   * missing frozen leaf may later become an expected edge. */
  for(UINTN i=0;i<Graph->count;i++) {
    const EDGE *e=&Graph->edges[i];
    if(!Guid(&e->protocol,&UsbHcGuid)||e->attributes!=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER||
      EdgeMatches(&Baseline,e))continue;
    if(UsbNewCount==MAX_USB_LEAVES){UsbNewOmitted++;continue;}
    USB_NEW_LEAF *candidate=&UsbNewLeaves[UsbNewCount++];
    candidate->edge=*e;candidate->index=i;candidate->reason="shape";
    if(e->count!=1||!e->controller||e->controller==e->handle||e->controller==e->agent)continue;
    candidate->reason="protected-or-known-handle";
    if(UsbProtected(e->controller)||HandleIn(&Baseline,e->controller)||!HandleIn(Graph,e->controller))continue;
    candidate->reason="path";
    if(!UsbPathCopy(e->controller,candidate->path,&candidate->path_bytes))continue;
    candidate->reason="active-use";
    if(!UsbUnused(Graph,e->controller,i))continue;
    VOID *value=NULL;candidate->reason="protocol-inventory";
    if(!UsbBareInterface(e->controller,&value))continue;
    candidate->usable=TRUE;candidate->reason="bare-current-leaf";
  }
}
static VOID UsbObserve(const GRAPH *Graph) {
  BOOLEAN needed=FALSE,blocked[MAX_USB_LEAVES]={0};
  memset(UsbNewLeaves,0,sizeof(UsbNewLeaves));UsbNewCount=0;UsbNewOmitted=0;
  for(UINTN i=0;i<UsbLeafCount;i++) {
    USB_LEAF *leaf=&UsbLeaves[i];leaf->allowed=FALSE;leaf->replacement=NULL;
    if(leaf->eligible&&!EdgeMatches(Graph,&Baseline.edges[leaf->edge]))needed=TRUE;
  }
  if(!needed||Map()!=EFI_SUCCESS)return;
  UsbNewObserve(Graph);
  for(UINTN i=0;i<UsbLeafCount;i++) {
    USB_LEAF *leaf=&UsbLeaves[i];const EDGE *e=&Baseline.edges[leaf->edge];
    if(!leaf->eligible||EdgeMatches(Graph,e)||UsbProtected(e->handle)||
      UsbProtected(e->agent)||UsbProtected(e->controller)||!HandleIn(Graph,e->handle)||
      !HandleIn(Graph,e->agent)||EdgeMatches(Graph,&leaf->owner)!=1||
      !UsbUnused(Graph,e->controller,MAX_EDGES))continue;
    VOID *value=NULL;UINT8 bytes[USB_PATH_BYTES];UINTN size=0;
    if(Interface(e->handle,&UsbHcGuid,&value)!=EFI_SUCCESS||value!=leaf->host_interface||
      !Readable(value,1)||!UsbPathCopy(e->handle,bytes,&size)||size!=leaf->host_bytes||
      !Equal(bytes,leaf->host_path,size))continue;
    /* Never query an absent/recycled child just because its old pointer was
     * saved. A still-present handle must retain both exact protocols, the same
     * interface pointer and its frozen complete path, and have no active use. */
    if(HandleIn(Graph,e->controller)&&(!UsbBareInterface(e->controller,&value)||
      value!=leaf->child_interface||!UsbPathCopy(e->controller,bytes,&size)||
      size!=leaf->child_bytes||!Equal(bytes,leaf->child_path,size)))continue;
    if(!HandleIn(Graph,e->controller)) {
      UINTN matches=0;USB_NEW_LEAF *match=NULL;
      for(UINTN j=0;j<UsbNewCount;j++) {
        USB_NEW_LEAF *candidate=&UsbNewLeaves[j];
        if(!candidate->usable||candidate->edge.handle!=e->handle||candidate->edge.agent!=e->agent||
          candidate->path_bytes!=leaf->child_bytes||!Equal(candidate->path,leaf->child_path,leaf->child_bytes))continue;
        matches++;match=candidate;
      }
      if(matches>1)continue;
      if(matches==1){leaf->replacement=match->edge.controller;match->reason="candidate-path-match";}
    }
    leaf->allowed=TRUE;
  }
  /* Source/ancestor/alias checks apply to the freshly observed replacement,
   * excluding only its own uniquely matched path. Another same-path handle
   * remains protected. The frozen baseline is never rewritten. */
  BOOLEAN observed=Map()==EFI_SUCCESS&&UsbImageUses(Graph,blocked)&&UsbPathUses(Graph,blocked);
  for(UINTN i=0;i<UsbLeafCount;i++) {
    USB_LEAF *leaf=&UsbLeaves[i];
    if(!observed||blocked[i])leaf->allowed=FALSE;
    if(leaf->replacement)for(UINTN j=0;j<UsbLeafCount;j++)
      if(i!=j&&UsbLeaves[j].replacement==leaf->replacement)leaf->allowed=FALSE;
  }
}
static EFI_STATUS UsbFreeze(VOID) {
  /* Re-observe the entire graph and freeze optional classifications together,
   * after GPU/display/code identities are known and before any release. */
  EFI_TPL previous=Gu.bs->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS s=Map();if(s==EFI_SUCCESS)s=SnapshotLocked(&Current);
  if(s==EFI_SUCCESS&&Current.count!=Baseline.count)s=EFI_NOT_READY;
  for(UINTN i=0;s==EFI_SUCCESS&&i<Baseline.count;i++)
    if(EdgeMatches(&Current,&Baseline.edges[i])!=1)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Map();
  for(UINTN i=0;s==EFI_SUCCESS&&i<Baseline.count&&UsbLeafCount<MAX_USB_LEAVES;i++) {
    const EDGE *e=&Baseline.edges[i];
    if(!Guid(&e->protocol,&UsbHcGuid)||e->attributes!=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER||e->count!=1||
      e->handle==e->controller||e->agent==e->controller||e->handle==e->agent||
      UsbProtected(e->handle)||UsbProtected(e->agent)||UsbProtected(e->controller)||
      !HandleIn(&Current,e->handle)||!HandleIn(&Current,e->agent)||!HandleIn(&Current,e->controller)||
      !UsbUnused(&Baseline,e->controller,i))continue;
    USB_LEAF leaf={0};leaf.edge=i;
    leaf.owner=(EDGE){e->handle,e->agent,e->handle,UsbHcGuid,EFI_OPEN_PROTOCOL_BY_DRIVER,1};
    if(EdgeMatches(&Baseline,&leaf.owner)!=1||
      Interface(e->handle,&UsbHcGuid,&leaf.host_interface)!=EFI_SUCCESS||!Readable(leaf.host_interface,1)||
      !UsbBareInterface(e->controller,&leaf.child_interface)||
      !UsbPathCopy(e->handle,leaf.host_path,&leaf.host_bytes)||
      !UsbPathCopy(e->controller,leaf.child_path,&leaf.child_bytes)||!UsbPaths(&leaf))continue;
    leaf.eligible=TRUE;UsbLeaves[UsbLeafCount++]=leaf;
  }
  if(s==EFI_SUCCESS&&UsbLeafCount) {
    BOOLEAN blocked[MAX_USB_LEAVES]={0};
    BOOLEAN observed=UsbImageUses(&Current,blocked)&&UsbPathUses(&Current,blocked);
    for(UINTN i=0;i<UsbLeafCount;i++)UsbLeaves[i].eligible=observed&&!blocked[i];
  }
  UsbFrozen=s==EFI_SUCCESS;
  Gu.bs->RestoreTPL(previous);
  /* Diagnostic bytes are immutable copies. File logging occurs only after
   * restoring TPL; it cannot alter a saved classification or observation. */
  if(s==EFI_SUCCESS&&GuLogReady()) {
    UINTN eligible=0;for(UINTN i=0;i<UsbLeafCount;i++)if(UsbLeaves[i].eligible)eligible++;
    GuLog("REHEARSAL_USB_LEAF_BASELINE candidates=%zu eligible=%zu limit=%u\n",
      (size_t)UsbLeafCount,(size_t)eligible,MAX_USB_LEAVES);
    static const char hex[]="0123456789abcdef";
    for(UINTN i=0;i<UsbLeafCount;i++) {
      USB_LEAF *leaf=&UsbLeaves[i];if(!leaf->eligible)continue;
      char host[USB_PATH_BYTES*2+1],child[USB_PATH_BYTES*2+1];
      for(UINTN n=0;n<leaf->host_bytes;n++){host[n*2]=hex[leaf->host_path[n]>>4];host[n*2+1]=hex[leaf->host_path[n]&15];}
      host[leaf->host_bytes*2]=0;
      for(UINTN n=0;n<leaf->child_bytes;n++){child[n*2]=hex[leaf->child_path[n]>>4];child[n*2+1]=hex[leaf->child_path[n]&15];}
      child[leaf->child_bytes*2]=0;
      GuLog("REHEARSAL_USB_LEAF_CLASSIFIED index=%zu host=%#llx child=%#llx host_path=%s child_path=%s\n",
        (size_t)i,(unsigned long long)(UINTN)Baseline.edges[leaf->edge].handle,
        (unsigned long long)(UINTN)Baseline.edges[leaf->edge].controller,host,child);
    }
  }
  return Result(s);
}
static USB_LEAF *UsbRetired(UINTN Edge) {
  for(UINTN i=0;i<UsbLeafCount;i++)if(UsbLeaves[i].edge==Edge&&UsbLeaves[i].eligible&&
    UsbLeaves[i].allowed&&!UsbLeaves[i].replacement)return &UsbLeaves[i];
  return NULL;
}
static EFI_HANDLE UsbReplacement(UINTN Edge) {
  for(UINTN i=0;i<UsbLeafCount;i++)if(UsbLeaves[i].edge==Edge&&UsbLeaves[i].eligible&&UsbLeaves[i].allowed)
    return UsbLeaves[i].replacement;
  return NULL;
}
static EFI_HANDLE Remap(EFI_HANDLE Handle) {
  if(P.stage>=4) {
    if(Handle==P.original_gop)return P.gop;
    if(Handle==P.original_edid)return P.edid;
  }
  return Handle;
}
static BOOLEAN Removed(const EDGE *E) {
  UINTN release=P.stage<=3?P.stage:6-P.stage;
  if(release>=1&&E->handle==P.original_gop&&E->agent==P.agents[0]&&Guid(&E->protocol,&TextGuid))return TRUE;
  if(release>=2&&E->handle==P.original_gop&&E->agent==P.agents[1]&&Guid(&E->protocol,&GopGuid))return TRUE;
  return release>=3&&E->handle==P.target&&E->agent==P.agents[2];
}
static VOID NoteEdge(const char *Kind,const EDGE *E) {
  if(!GuLogReady())return;
  GuLog("REHEARSAL_GRAPH_%s stage=%zu handle=%#llx agent=%#llx controller=%#llx attributes=%#x count=%u protocol=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
    Kind,(size_t)P.stage,(unsigned long long)(UINTN)E->handle,(unsigned long long)(UINTN)E->agent,
    (unsigned long long)(UINTN)E->controller,E->attributes,E->count,(unsigned)E->protocol.Data1,
    (unsigned)E->protocol.Data2,(unsigned)E->protocol.Data3,(unsigned)E->protocol.Data4[0],
    (unsigned)E->protocol.Data4[1],(unsigned)E->protocol.Data4[2],(unsigned)E->protocol.Data4[3],
    (unsigned)E->protocol.Data4[4],(unsigned)E->protocol.Data4[5],(unsigned)E->protocol.Data4[6],(unsigned)E->protocol.Data4[7]);
}
static VOID UsbPathHex(const UINT8 *Bytes,UINTN Size,char *Text) {
  static const char hex[]="0123456789abcdef";
  if(Size>USB_PATH_BYTES)Size=0;
  for(UINTN i=0;i<Size;i++){Text[i*2]=hex[Bytes[i]>>4];Text[i*2+1]=hex[Bytes[i]&15];}Text[Size*2]=0;
}
static VOID NoteUsbNewLeaves(VOID) {
  if(!GuLogReady()||(!UsbNewCount&&!UsbNewOmitted))return;
  UINTN count=UsbNewCount<16?UsbNewCount:16;
  for(UINTN i=0;i<count;i++) {
    USB_NEW_LEAF *candidate=&UsbNewLeaves[i];char path[USB_PATH_BYTES*2+1];
    UsbPathHex(candidate->path,candidate->path_bytes,path);
    GuLog("REHEARSAL_USB_NEW_CHILD index=%zu host=%#llx agent=%#llx child=%#llx count=%u reason=%s path=%s\n",
      (size_t)i,(unsigned long long)(UINTN)candidate->edge.handle,(unsigned long long)(UINTN)candidate->edge.agent,
      (unsigned long long)(UINTN)candidate->edge.controller,candidate->edge.count,candidate->reason,path);
  }
  GuLog("REHEARSAL_USB_NEW_CHILD_SUMMARY captured=%zu omitted=%zu\n",
    (size_t)count,(size_t)(UsbNewCount-count+UsbNewOmitted));
}
static VOID NoteUsbRetirements(VOID) {
  if(!GuLogReady())return;
  for(UINTN i=0;i<UsbLeafCount;i++) {
    USB_LEAF *leaf=&UsbLeaves[i];
    if(leaf->pending&&!leaf->logged){leaf->logged=TRUE;NoteEdge("USB_LEAF_RETIRED",&Baseline.edges[leaf->edge]);}
    if(leaf->pending_replacement&&leaf->pending_replacement!=leaf->logged_replacement) {
      /* The successful prior snapshot queued the numeric handle. Never infer
       * it from a later observation or dereference it while logging. */
      leaf->logged_replacement=leaf->pending_replacement;
      if(UsbReplacementLogs++<MAX_USB_LEAVES) {
        char path[USB_PATH_BYTES*2+1];UsbPathHex(leaf->child_path,leaf->child_bytes,path);
        const EDGE *e=&Baseline.edges[leaf->edge];
        GuLog("REHEARSAL_GRAPH_USB_LEAF_REPLACED stage=%zu host=%#llx agent=%#llx old=%#llx new=%#llx child_path=%s\n",
          (size_t)P.stage,(unsigned long long)(UINTN)e->handle,(unsigned long long)(UINTN)e->agent,
          (unsigned long long)(UINTN)e->controller,(unsigned long long)(UINTN)leaf->pending_replacement,path);
      }
    }
  }
}
static EFI_STATUS Expected(VOID) {
  UINTN expected=0,missing=0,unexpected=0,retired=0,replaced=0,printed=0;
  /* Evaluate every edge, while limiting refusal logging to 64 entries. Logs
   * describe this immutable snapshot; omitted differences remain refusals. */
  for(UINTN i=0;i<Baseline.count;i++) {
    const EDGE *old=&Baseline.edges[i];if(Removed(old))continue;
    EDGE e=*old;EFI_HANDLE replacement=UsbReplacement(i);
    if(replacement){e.controller=replacement;replaced++;}
    e.handle=Remap(e.handle);e.agent=Remap(e.agent);e.controller=Remap(e.controller);
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&e,&Current.edges[j]))matches++;
    if(!matches&&UsbRetired(i)){retired++;continue;}
    if(matches!=1){missing++;if(printed++<64)NoteEdge(matches?"DUPLICATE":"MISSING",&e);}expected++;
  }
#if GU_STAGED_UNLOCK
  if(P.claimed) {
    EDGE self={P.target,Gu.image,NULL,PciGuid,EFI_OPEN_PROTOCOL_EXCLUSIVE,1};UINTN matches=0;
    for(UINTN i=0;i<Current.count;i++)if(SameEdge(&self,&Current.edges[i]))matches++;
    if(matches!=1){missing++;if(printed++<64)NoteEdge(matches?"DUPLICATE":"MISSING",&self);}expected++;
  }
#endif
  {
    for(UINTN j=0;j<Current.count;j++) {
      BOOLEAN found=ClaimEdge(&Current.edges[j]);
      for(UINTN i=0;i<Baseline.count;i++) {
        EDGE e=Baseline.edges[i];if(Removed(&e))continue;
        EFI_HANDLE replacement=UsbReplacement(i);if(replacement)e.controller=replacement;
        e.handle=Remap(e.handle);e.agent=Remap(e.agent);e.controller=Remap(e.controller);
        if(SameEdge(&e,&Current.edges[j])){found=TRUE;break;}
      }
      if(!found){unexpected++;if(printed++<64)NoteEdge("UNEXPECTED",&Current.edges[j]);}
    }
  }
  if(missing||unexpected||expected!=Current.count) {
    if(GuLogReady())GuLog("REHEARSAL_GRAPH_DELTA missing=%zu unexpected=%zu retired=%zu expected=%zu actual=%zu omitted=%zu replaced=%zu\n",
      (size_t)missing,(size_t)unexpected,(size_t)retired,(size_t)expected,(size_t)Current.count,
      (size_t)(printed>64?printed-64:0),(size_t)replaced);
    NoteUsbNewLeaves();
    return EFI_NOT_READY;
  }
  /* Queue success diagnostics. Verify emits them before its NEXT observation,
   * never between the final authorizing snapshot and a driver callback. */
  for(UINTN i=0;i<UsbLeafCount;i++)if(UsbLeaves[i].allowed) {
    if(UsbLeaves[i].replacement)UsbLeaves[i].pending_replacement=UsbLeaves[i].replacement;
    else UsbLeaves[i].pending=TRUE;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS Console(BOOLEAN First) {
  EFI_SYSTEM_TABLE *t=P.table;
  if(!t->ConsoleOutHandle||t->ConsoleOutHandle!=t->ConsoleInHandle||
    t->ConsoleOutHandle!=t->StandardErrorHandle||!Readable(t->ConOut,sizeof(*t->ConOut))||
    !Readable(t->StdErr,sizeof(*t->StdErr))||!Readable(t->ConIn,sizeof(*t->ConIn)))return EFI_UNSUPPORTED;
  if(!t->ConIn->Reset||!t->ConIn->ReadKeyStroke||!t->ConIn->WaitForKey||
    !t->ConOut->Reset||!t->ConOut->OutputString||!t->ConOut->SetMode)return EFI_UNSUPPORTED;
  VOID *out=NULL,*in=NULL,*err=NULL;
  EFI_STATUS s=Interface(t->ConsoleOutHandle,&TextGuid,&out);
  if(s==EFI_SUCCESS)s=Interface(t->ConsoleInHandle,&InputGuid,&in);
  if(s==EFI_SUCCESS)s=Interface(t->StandardErrorHandle,&TextGuid,&err);
  if(s!=EFI_SUCCESS||out!=t->ConOut||in!=t->ConIn||err!=t->StdErr)return EFI_NOT_READY;
  if(First) {
    P.console=t->ConsoleOutHandle;P.out=t->ConOut;P.err=t->StdErr;P.in=t->ConIn;
    P.out_callbacks=*t->ConOut;P.err_callbacks=*t->StdErr;P.in_callbacks=*t->ConIn;
  } else if(t->ConsoleOutHandle!=P.console||t->ConOut!=P.out||t->StdErr!=P.err||t->ConIn!=P.in||
    !Equal(t->ConOut,&P.out_callbacks,sizeof(P.out_callbacks))||
    !Equal(t->StdErr,&P.err_callbacks,sizeof(P.err_callbacks))||
    !Equal(t->ConIn,&P.in_callbacks,sizeof(P.in_callbacks)))return EFI_NOT_READY;
  return Inventory(P.console,GuRehearsalConsoleProtocols,COUNT(GuRehearsalConsoleProtocols));
}
typedef struct {
  EFI_PCI_IO_PROTOCOL *interface,api;
  EFI_STATUS interface_status,location_status,config_status,attributes_status,bar_status;
  UINTN segment,bus,device,function;
  UINT32 config[64],missing;
  UINT16 cleared_status,status_offset,pcie_capability;
  UINT64 attributes,expected_attributes,decode;
  BOOLEAN api_valid,location_valid,config_valid,attributes_valid;
} CONFIG_OBSERVATION;
static EFI_STATUS NoteConfig(const CONFIG_OBSERVATION *Observation,const char *Reason,
  EFI_STATUS Status,BOOLEAN First,BOOLEAN PostFlr) {
  Status=Result(Status);
  if((Status==EFI_SUCCESS&&!PostFlr)||!GuLogReady())return Status;
  /* Copy every bounded observation before file logging. No callback, config
   * read, allocation, or stale interface dereference is used to diagnose a
   * refusal. Config is called at application TPL; containment uses its separate
   * silent refresh and never enters this logger. */
  CONFIG_OBSERVATION o=*Observation;
  UINT32 baseline[64];memcpy(baseline,P.config,sizeof(baseline));
  EFI_PCI_IO_PROTOCOL expected=P.pci_api;
  EFI_PCI_IO_PROTOCOL *expected_interface=P.pci,*global_interface=Gu.pci;
  UINT64 baseline_attributes=P.attributes;
  UINTN stage=P.stage;BOOLEAN baseline_valid=!First&&P.pci!=NULL;
#if GU_STAGED_UNLOCK
  if(PostFlr)P.post_config_logged=TRUE;
#endif
#define CONFIG_LOG(...) do { \
  if(!GuLogReady())goto log_failed; \
  GuLog(__VA_ARGS__); \
  if(!GuLogReady()||Gu.fault)goto log_failed; \
} while(0)
  CONFIG_LOG("REHEARSAL_PCI_CHECK stage=%zu reason=%s status=%#llx first=%u post_flr=%u config_valid=%u attributes_valid=%u\n",
    (size_t)stage,Reason,(unsigned long long)Status,First,PostFlr,o.config_valid,o.attributes_valid);
  CONFIG_LOG("REHEARSAL_PCI_TRANSPORT interface=%#llx location=%#llx config_read=%#llx attributes=%#llx bar=%#llx\n",
    (unsigned long long)o.interface_status,(unsigned long long)o.location_status,
    (unsigned long long)o.config_status,(unsigned long long)o.attributes_status,(unsigned long long)o.bar_status);
  CONFIG_LOG("REHEARSAL_PCI_INTERFACE expected=%#llx current_global=%#llx actual=%#llx readable=%u table_same=%u\n",
    (unsigned long long)(UINTN)expected_interface,(unsigned long long)(UINTN)global_interface,
    (unsigned long long)(UINTN)o.interface,o.api_valid,
    baseline_valid&&o.api_valid&&Equal(&expected,&o.api,sizeof(expected)));
  CONFIG_LOG("REHEARSAL_PCI_LOCATION valid=%u segment=%zu bus=%zu device=%zu function=%zu\n",
    o.location_valid,(size_t)o.segment,(size_t)o.bus,(size_t)o.device,(size_t)o.function);
  CONFIG_LOG("REHEARSAL_PCI_ATTRIBUTES valid=%u baseline=%#llx expected=%#llx actual=%#llx decode_expected=%#llx missing_normalized=%#x\n",
    o.attributes_valid,(unsigned long long)baseline_attributes,(unsigned long long)o.expected_attributes,
    (unsigned long long)o.attributes,(unsigned long long)o.decode,o.missing);
  if(baseline_valid&&o.config_valid&&o.pcie_capability>=0x40&&o.pcie_capability<=0xd4&&!(o.pcie_capability&3)) {
    UINTN offset=(UINTN)o.pcie_capability+0x0a,word=offset/4,shift=(offset&3)*8;
    UINT16 before=(UINT16)(baseline[word]>>shift),after=(UINT16)(o.config[word]>>shift);
    CONFIG_LOG("REHEARSAL_PCI_DEVICE_STATUS offset=%#x baseline=%#x current=%#x xor=%#x current_errors=%#x pending=%u\n",
      (unsigned)offset,before,after,before^after,after&0x000fU,(after&0x0020U)!=0);
  }
  if(PostFlr&&Status==EFI_SUCCESS&&o.cleared_status&&baseline_valid&&o.config_valid) {
    UINTN word=o.status_offset/4,shift=(o.status_offset&3)*8;
    CONFIG_LOG("REHEARSAL_PCI_STATUS_CLEAR_ACCEPTED stage=%zu offset=%#x baseline=%#x current=%#x cleared=%#x allowed_clear=0x9\n",
      (size_t)stage,o.status_offset,(unsigned)((baseline[word]>>shift)&0xffffU),
      (unsigned)((o.config[word]>>shift)&0xffffU),o.cleared_status);
  }
  if(PostFlr&&baseline_valid)for(UINTN i=0;i<COUNT(baseline);i+=4)
    CONFIG_LOG("REHEARSAL_PCI_SNAPSHOT source=baseline offset=%#x values=%08x/%08x/%08x/%08x\n",
      (unsigned)(i*4),baseline[i],baseline[i+1],baseline[i+2],baseline[i+3]);
  if(PostFlr&&o.config_valid)for(UINTN i=0;i<COUNT(o.config);i+=4)
    CONFIG_LOG("REHEARSAL_PCI_SNAPSHOT source=current offset=%#x values=%08x/%08x/%08x/%08x\n",
      (unsigned)(i*4),o.config[i],o.config[i+1],o.config[i+2],o.config[i+3]);
  if(baseline_valid&&o.config_valid)for(UINTN i=0;i<COUNT(o.config);i++)
    if(baseline[i]!=o.config[i])
      CONFIG_LOG("REHEARSAL_PCI_DIFF offset=%#x expected=%08x actual=%08x xor=%08x\n",
        (unsigned)(i*4),baseline[i],o.config[i],baseline[i]^o.config[i]);
  if(baseline_valid&&o.api_valid) {
#define PCI_API_FIELD(Field) {#Field,__builtin_offsetof(EFI_PCI_IO_PROTOCOL,Field),sizeof(expected.Field)}
    static const struct {const char *name;UINTN offset,bytes;} fields[]={
      PCI_API_FIELD(PollMem),PCI_API_FIELD(PollIo),PCI_API_FIELD(Mem.Read),PCI_API_FIELD(Mem.Write),
      PCI_API_FIELD(Io.Read),PCI_API_FIELD(Io.Write),PCI_API_FIELD(Pci.Read),PCI_API_FIELD(Pci.Write),
      PCI_API_FIELD(CopyMem),PCI_API_FIELD(Map),PCI_API_FIELD(Unmap),PCI_API_FIELD(AllocateBuffer),
      PCI_API_FIELD(FreeBuffer),PCI_API_FIELD(Flush),PCI_API_FIELD(GetLocation),PCI_API_FIELD(Attributes),
      PCI_API_FIELD(GetBarAttributes),PCI_API_FIELD(SetBarAttributes),PCI_API_FIELD(RomSize),PCI_API_FIELD(RomImage)};
#undef PCI_API_FIELD
    for(UINTN i=0;i<COUNT(fields);i++) {
      UINT64 before=0,after=0;UINTN n=fields[i].bytes,off=fields[i].offset;
      if(n>sizeof(before)||off>sizeof(expected)||n>sizeof(expected)-off)continue;
      memcpy(&before,(UINT8 *)&expected+off,n);memcpy(&after,(UINT8 *)&o.api+off,n);
      if(before!=after)CONFIG_LOG("REHEARSAL_PCI_API_DIFF field=%s offset=%zu expected=%#llx actual=%#llx\n",
        fields[i].name,(size_t)off,(unsigned long long)before,(unsigned long long)after);
    }
  }
#undef CONFIG_LOG
  return Status;
log_failed:
  return Status==EFI_SUCCESS?EFI_DEVICE_ERROR:Status;
}
static EFI_STATUS Config(BOOLEAN First) {
  EFI_PCI_IO_PROTOCOL *io=NULL;UINT32 c[64];UINTN segment,bus,device,function;UINT64 attrs=0;
  CONFIG_OBSERVATION o={0};BOOLEAN post=FALSE;
  o.interface_status=o.location_status=o.config_status=o.attributes_status=o.bar_status=EFI_NOT_STARTED;
  o.expected_attributes=P.attributes;o.pcie_capability=Gu.pcie_cap;
#if GU_STAGED_UNLOCK
  post=!First&&P.prepared&&P.stage==3&&!P.post_config_logged&&TransactionClean();
#endif
#define CONFIG_RETURN(Reason,Status) return NoteConfig(&o,Reason,Status,First,post)
  /* Preserve Interface()'s exact contract while retaining the raw EFI status. */
  o.interface_status=Gu.bs->HandleProtocol(P.target,&PciGuid,(VOID **)&io);o.interface=io;
  EFI_STATUS s=o.interface_status==EFI_SUCCESS&&!io?EFI_COMPROMISED_DATA:Result(o.interface_status);
  if(s==EFI_SUCCESS&&Readable(io,sizeof(*io))){o.api=*io;o.api_valid=TRUE;}
  if(s!=EFI_SUCCESS){CONFIG_RETURN("interface-status",EFI_NOT_READY);}
  if(io!=Gu.pci){CONFIG_RETURN("interface-pointer",EFI_NOT_READY);}
  if(!Readable(io,sizeof(*io))){CONFIG_RETURN("interface-unreadable",EFI_NOT_READY);}
  if(!io->Pci.Read||!io->GetLocation||!io->Attributes||!io->GetBarAttributes){CONFIG_RETURN("interface-callback",EFI_NOT_READY);}
  if(!First&&!Equal(io,&P.pci_api,sizeof(*io))){CONFIG_RETURN("interface-table",EFI_NOT_READY);}
  s=io->GetLocation(io,&segment,&bus,&device,&function);o.location_status=s;
  if(s!=EFI_SUCCESS){CONFIG_RETURN("location-status",EFI_UNSUPPORTED);}
  o.location_valid=TRUE;o.segment=segment;o.bus=bus;o.device=device;o.function=function;
  s=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,c);o.config_status=s;
  if(s!=EFI_SUCCESS){CONFIG_RETURN("config-read",EFI_UNSUPPORTED);}
  memcpy(o.config,c,sizeof(c));o.config_valid=TRUE;
  o.decode=(c[1]&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(c[1]&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  if(!GuTargetConfig(c,segment,bus,device,function)){CONFIG_RETURN("identity",EFI_UNSUPPORTED);}
  if(c[1]&4){CONFIG_RETURN("bme",EFI_UNSUPPORTED);}
  s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);o.attributes_status=s;
  if(s!=EFI_SUCCESS){CONFIG_RETURN("attributes-read",Result(s));}
  o.attributes=attrs;o.attributes_valid=TRUE;
  UINT64 decode=o.decode;
  if((attrs&(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))!=decode){CONFIG_RETURN("decode",EFI_NOT_READY);}
  if(First){memcpy(P.config,c,sizeof(c));P.attributes=attrs;P.pci=io;P.pci_api=*io;}
  else {
    if(io!=P.pci){CONFIG_RETURN("interface-pointer",EFI_NOT_READY);}
    BOOLEAN allow_missing=P.stage==3;
#if GU_STAGED_UNLOCK
    if(P.claim_done)allow_missing=FALSE;
#endif
    UINT32 missing=allow_missing?(P.config[1]&~c[1]&3U):0;o.missing=missing;
    c[1]|=missing;
    UINT64 absent=(missing&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(missing&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
    o.expected_attributes=P.attributes&~absent;
    if(!GuConfigEqual(P.config,c,Gu.pcie_cap,&o.cleared_status)){CONFIG_RETURN("raw-config",EFI_NOT_READY);}
    if(o.cleared_status)o.status_offset=Gu.pcie_cap+0x0a;
    if(attrs!=(P.attributes&~absent)){CONFIG_RETURN("raw-attributes",EFI_NOT_READY);}
  }
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *bar=NULL;
  s=io->GetBarAttributes(io,0,NULL,(VOID **)&bar);o.bar_status=s;
  const char *reason=s==EFI_SUCCESS?"verified":"bar-query";
  if(s==EFI_SUCCESS&&(!bar||bar->Desc!=0x8a||bar->Len!=43||bar->ResType||
    bar->AddrRangeMin!=Gu.bar_base||bar->AddrLen!=Gu.bar_size||bar->AddrTranslationOffset||
    (bar->AddrRangeMax!=Gu.bar_base+Gu.bar_size-1&&bar->AddrRangeMax!=Gu.bar_size-1))) {
    s=EFI_UNSUPPORTED;reason="bar-descriptor";
  }
  EFI_STATUS freed=Free(bar,s);if(s==EFI_SUCCESS&&freed!=EFI_SUCCESS)reason="bar-free";
  CONFIG_RETURN(reason,freed);
#undef CONFIG_RETURN
}
static EFI_STATUS ExactPciState(UINT32 ConfigBytes[64],UINT64 *Attributes) {
  EFI_STATUS s=Map();if(s!=EFI_SUCCESS)return s;
  EFI_PCI_IO_PROTOCOL *io=NULL;
  s=Interface(P.target,&PciGuid,(VOID **)&io);
  if(s!=EFI_SUCCESS||io!=P.pci||!Readable(io,sizeof(*io))||!Equal(io,&P.pci_api,sizeof(*io)))return EFI_NOT_READY;
  s=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,ConfigBytes);
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,Attributes);
  return Result(s);
}
static EFI_STATUS ResolveChildren(BOOLEAN First) {
  EFI_HANDLE *handles=NULL;UINTN count=0;EFI_HANDLE gop=NULL,edid=NULL;
  EFI_STATUS s=Gu.bs->LocateHandleBuffer(ByProtocol,&PathGuid,NULL,&count,&handles);
  if(s==EFI_SUCCESS&&(!handles||!count||count>MAX_HANDLES))s=EFI_BAD_BUFFER_SIZE;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    if(Path(handles[i],GuRehearsalGopPath,sizeof(GuRehearsalGopPath))) {
      if(gop){s=EFI_NO_MAPPING;break;}gop=handles[i];
    }
    if(Path(handles[i],GuRehearsalEdidPath,sizeof(GuRehearsalEdidPath))) {
      if(edid){s=EFI_NO_MAPPING;break;}edid=handles[i];
    }
  }
  s=Free(handles,s);
  if(s!=EFI_SUCCESS)return Result(s);
  if(!gop||!edid||gop==edid||gop==P.target||edid==P.target||gop==P.console||edid==P.console||gop==Gu.image||edid==Gu.image||
    (!First&&(gop==P.original_edid||edid==P.original_gop)))return EFI_UNSUPPORTED;
  EFI_HANDLE children[]={gop,edid};
  for(UINTN i=0;i<2;i++) {
    EFI_DEVICE_PATH_PROTOCOL *path=NULL;EFI_HANDLE parent=NULL;
    s=Interface(children[i],&PathGuid,(VOID **)&path);
    if(s==EFI_SUCCESS)s=Gu.bs->LocateDevicePath(&PciGuid,&path,&parent);
    if(s!=EFI_SUCCESS||parent!=P.target)return EFI_UNSUPPORTED;
    if(!First&&HandleIn(&Baseline,children[i])&&children[i]!=P.original_gop&&children[i]!=P.original_edid)return EFI_NOT_READY;
    for(UINTN d=0;d<3;d++)if(children[i]==P.agents[d]||children[i]==P.images[d])return EFI_NOT_READY;
  }
  P.gop=gop;P.edid=edid;
  if(First){P.original_gop=gop;P.original_edid=edid;}
  return EFI_SUCCESS;
}
/* Observation only: never call QueryMode, SetMode or Blt to diagnose a refusal.
 * Copy only mapped, fixed-size structures before logging. In particular, an
 * absent/uninitialized mode must not turn the failure report into a bad read. */
static VOID NoteGop(const char *Reason,EFI_HANDLE Handle,EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop) {
  if(!GuLogReady())return;
  EFI_GRAPHICS_OUTPUT_PROTOCOL g={0};EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE m={0};
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info={0};
  BOOLEAN readable=Readable(Gop,sizeof(g)),mode_readable=FALSE,info_readable=FALSE;
  if(readable) {
    memcpy(&g,Gop,sizeof(g));mode_readable=Readable(g.Mode,sizeof(m));
    if(mode_readable) {
      memcpy(&m,g.Mode,sizeof(m));
      info_readable=m.SizeOfInfo>=sizeof(info)&&Readable(m.Info,sizeof(info));
      if(info_readable)memcpy(&info,m.Info,sizeof(info));
    }
  }
  GuLog("REHEARSAL_GOP_OBSERVATION stage=%zu reason=%s handle=%#llx expected=%#llx original=%#llx console=%#llx interface=%#llx readable=%u\n",
    (size_t)P.stage,Reason,(unsigned long long)(UINTN)Handle,(unsigned long long)(UINTN)P.gop,
    (unsigned long long)(UINTN)P.original_gop,(unsigned long long)(UINTN)P.console,
    (unsigned long long)(UINTN)Gop,(unsigned)readable);
  if(!readable)return;
  GuLog("REHEARSAL_GOP_CALLBACKS stage=%zu query=%#llx set=%#llx blt=%#llx mode_ptr=%#llx mode_readable=%u\n",
    (size_t)P.stage,(unsigned long long)(UINTN)g.QueryMode,(unsigned long long)(UINTN)g.SetMode,
    (unsigned long long)(UINTN)g.Blt,(unsigned long long)(UINTN)g.Mode,(unsigned)mode_readable);
  if(!mode_readable)return;
  GuLog("REHEARSAL_GOP_MODE stage=%zu max=%u mode=%u info_ptr=%#llx info_bytes=%zu info_readable=%u framebuffer=%#llx framebuffer_bytes=%zu\n",
    (size_t)P.stage,m.MaxMode,m.Mode,(unsigned long long)(UINTN)m.Info,(size_t)m.SizeOfInfo,
    (unsigned)info_readable,(unsigned long long)m.FrameBufferBase,(size_t)m.FrameBufferSize);
  if(!info_readable)return;
  GuLog("REHEARSAL_GOP_INFO stage=%zu version=%u width=%u height=%u format=%u stride=%u masks=%#x/%#x/%#x/%#x\n",
    (size_t)P.stage,info.Version,info.HorizontalResolution,info.VerticalResolution,(unsigned)info.PixelFormat,
    info.PixelsPerScanLine,info.PixelInformation.RedMask,info.PixelInformation.GreenMask,
    info.PixelInformation.BlueMask,info.PixelInformation.ReservedMask);
}
static VOID DisplayRefused(const char *Reason,EFI_STATUS Status,EFI_HANDLE Handle,EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop) {
  if(!GuLogReady())return;
  GuLog("REHEARSAL_DISPLAY_REFUSED stage=%zu reason=%s status=%#llx\n",
    (size_t)P.stage,Reason,(unsigned long long)Status);
  NoteGop(Reason,Handle,Gop);
}
static BOOLEAN PinnedActiveOutput(const EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop) {
  if(!GuRehearsalDeferredEnabled)return TRUE;
  const EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *m=Gop->Mode;
  return GuRehearsalDeferredMaxMode&&
    GuRehearsalDeferredActiveMode<GuRehearsalDeferredMaxMode&&
    GuRehearsalDeferredMode>=GuRehearsalDeferredMaxMode&&
    m->MaxMode==GuRehearsalDeferredMaxMode&&m->Mode==GuRehearsalDeferredActiveMode&&
    m->SizeOfInfo==GuRehearsalDeferredInfoBytes&&
    m->FrameBufferBase==GuRehearsalDeferredFrameBufferBase&&
    m->FrameBufferSize==GuRehearsalDeferredFrameBufferBytes&&
    Equal(m->Info,&GuRehearsalDeferredInfo,sizeof(GuRehearsalDeferredInfo));
}
static BOOLEAN DeferredOutput(const EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop) {
  /* UEFI permits framebuffer initialization to wait for SetMode. This is only
   * the exact uninitialized state observed after this pinned NVIDIA driver's
   * Start, before the pinned GraphicsConsole driver gets its turn. It is never
   * sufficient at stage 5 or later, and does not bypass graph/driver checks. */
  const EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *m=Gop->Mode;
  return GuRehearsalDeferredEnabled&&P.stage==4&&
    Gop->QueryMode==P.gop_callbacks.QueryMode&&Gop->SetMode==P.gop_callbacks.SetMode&&
    Gop->Blt==P.gop_callbacks.Blt&&m->MaxMode==GuRehearsalDeferredMaxMode&&
    m->MaxMode==P.mode.MaxMode&&m->Mode==GuRehearsalDeferredMode&&
    m->SizeOfInfo==GuRehearsalDeferredInfoBytes&&m->SizeOfInfo==P.mode.SizeOfInfo&&
    !m->FrameBufferBase&&!m->FrameBufferSize&&
    Equal(m->Info,&GuRehearsalDeferredInfo,sizeof(GuRehearsalDeferredInfo))&&
    Equal(m->Info,&P.info,sizeof(P.info));
}
static BOOLEAN SameStage4Output(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop) {
  /* Supported must not initialize or replace the display state. Pin either
   * valid active state or the exact deferred state immediately after NVIDIA
   * Start, then require that same state until GraphicsConsole Start is called. */
  if(!P.stage4_seen) {
    P.stage4_gop=Gop;P.stage4_callbacks=*Gop;P.stage4_mode=*Gop->Mode;
    P.stage4_info=*Gop->Mode->Info;P.stage4_seen=TRUE;return TRUE;
  }
  return Gop==P.stage4_gop&&Equal(Gop,&P.stage4_callbacks,sizeof(P.stage4_callbacks))&&
    Equal(Gop->Mode,&P.stage4_mode,sizeof(P.stage4_mode))&&
    Equal(Gop->Mode->Info,&P.stage4_info,sizeof(P.stage4_info));
}
static EFI_STATUS Output(BOOLEAN First) {
  EFI_HANDLE *handles=NULL;UINTN count=0,physical=0;
  EFI_STATUS s=Gu.bs->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&handles);
  if(P.stage==3&&s==EFI_NOT_FOUND)return EFI_SUCCESS;
  if(s==EFI_SUCCESS&&(count>128||(count&&!handles)))s=EFI_BAD_BUFFER_SIZE;
  if(s!=EFI_SUCCESS)DisplayRefused("enumerate",s,NULL,NULL);
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;
    const char *reason=NULL;
    s=Interface(handles[i],&GopGuid,(VOID **)&g);
    if(s!=EFI_SUCCESS){reason="interface";g=NULL;}
    else if(!Readable(g,sizeof(*g)))reason="protocol-unreadable";
    else if(!Readable(g->Mode,sizeof(*g->Mode)))reason="mode-unreadable";
    if(reason){s=EFI_COMPROMISED_DATA;DisplayRefused(reason,s,handles[i],g);break;}
    if(handles[i]==P.console&&!g->Mode->FrameBufferBase&&!g->Mode->FrameBufferSize) {
      if(First||P.stage==4)NoteGop("virtual-zero-framebuffer",handles[i],g);
      continue;
    }
    if(First)NoteGop("baseline",handles[i],g);
    /* Check identity and readable structures before recognizing the narrowly
     * pinned stage-4 deferred state. All other stages require an active mode. */
    if(P.stage==3)reason="detached-physical";
    else if(handles[i]!=P.gop)reason="unexpected-handle";
    else if(++physical!=1)reason="duplicate-physical";
    else if(!g->QueryMode||!g->SetMode||!g->Blt)reason="missing-callback";
    else if(!Readable(g->Mode->Info,sizeof(*g->Mode->Info)))reason="info-unreadable";
    else if(g->Mode->SizeOfInfo<sizeof(*g->Mode->Info))reason="info-size";
    if(reason){s=EFI_UNSUPPORTED;DisplayRefused(reason,s,handles[i],g);break;}
    if(!First&&DeferredOutput(g)) {
      if(!SameStage4Output(g)) {
        s=EFI_NOT_READY;DisplayRefused("stage4-state-changed",s,handles[i],g);break;
      }
      if(GuLogReady())GuLog("REHEARSAL_GOP_DEFERRED stage=4 accepted=1 mode=%u max=%u framebuffer=0 framebuffer_bytes=0\n",
        g->Mode->Mode,g->Mode->MaxMode);
      continue;
    }
    if(!g->Mode->FrameBufferBase)reason="zero-framebuffer-base";
    else if(!g->Mode->FrameBufferSize)reason="zero-framebuffer-size";
    if(reason){s=EFI_UNSUPPORTED;DisplayRefused(reason,s,handles[i],g);break;}
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info=g->Mode->Info;
    if(!info->HorizontalResolution||!info->VerticalResolution)reason="zero-resolution";
    else if(info->PixelFormat>PixelBlueGreenRedReserved8BitPerColor)reason="pixel-format";
    else if(info->PixelsPerScanLine<info->HorizontalResolution)reason="scanline";
    else if((UINT64)info->PixelsPerScanLine*info->VerticalResolution>g->Mode->FrameBufferSize/4)reason="framebuffer-capacity";
    if(reason){s=EFI_UNSUPPORTED;DisplayRefused(reason,s,handles[i],g);break;}
    if(First) {
      if(!PinnedActiveOutput(g)) {
        s=EFI_NOT_READY;DisplayRefused("evidence-baseline-mismatch",s,handles[i],g);break;
      }
      P.gop_interface=g;P.gop_callbacks=*g;P.mode=*g->Mode;P.info=*info;
    }
    else if((P.stage<3&&g!=P.gop_interface)||g->QueryMode!=P.gop_callbacks.QueryMode||
      g->SetMode!=P.gop_callbacks.SetMode||g->Blt!=P.gop_callbacks.Blt||
      g->Mode->MaxMode!=P.mode.MaxMode||g->Mode->Mode!=P.mode.Mode||
      g->Mode->SizeOfInfo!=P.mode.SizeOfInfo||g->Mode->FrameBufferBase!=P.mode.FrameBufferBase||
      g->Mode->FrameBufferSize!=P.mode.FrameBufferSize||!Equal(info,&P.info,sizeof(P.info))) {
      s=EFI_NOT_READY;DisplayRefused("baseline-mismatch",s,handles[i],g);
    }
    if(s==EFI_SUCCESS&&P.stage==4&&!SameStage4Output(g)) {
      s=EFI_NOT_READY;DisplayRefused("stage4-state-changed",s,handles[i],g);
    }
  }
  s=Free(handles,s);
  if(s==EFI_SUCCESS&&physical!=(P.stage==3?0U:1U)) {
    s=EFI_NOT_READY;DisplayRefused("physical-count",s,NULL,NULL);
  }
  return Result(s);
}
static EFI_STATUS Driver(UINTN Which,BOOLEAN First,EFI_DRIVER_BINDING_PROTOCOL **Validated) {
  if(Validated)*Validated=NULL;
  const GU_REHEARSAL_DRIVER_PIN *pin=&GuRehearsalDrivers[Which];
  if(!pin->image_bytes||pin->image_bytes>GU_PE_MAX_IMAGE||!pin->file_path_bytes||
    pin->file_path_bytes>sizeof(pin->file_path)||pin->code_rva>=pin->image_bytes||
    !pin->code_bytes||pin->code_bytes>pin->image_bytes-pin->code_rva)return EFI_UNSUPPORTED;
  EFI_STATUS mapped=Map();if(mapped!=EFI_SUCCESS)return mapped;
  EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;EFI_LOADED_IMAGE_PROTOCOL *im=NULL;
  EFI_STATUS s=Interface(P.agents[Which],&BindingGuid,(VOID **)&binding);
  if(s!=EFI_SUCCESS||!Readable(binding,sizeof(*binding))||binding->DriverBindingHandle!=P.agents[Which]||
    !binding->ImageHandle||binding->Version!=pin->version)return EFI_UNSUPPORTED;
  s=Interface(binding->ImageHandle,&ImageGuid,(VOID **)&im);
  if(s!=EFI_SUCCESS||!Readable(im,sizeof(*im))||!im->ImageBase||im->ImageSize!=pin->image_bytes||
    im->ImageCodeType!=EfiBootServicesCode||im->ImageDataType!=EfiBootServicesData||
    !Readable(im->ImageBase,(UINTN)im->ImageSize)||!Readable(im->FilePath,pin->file_path_bytes)||
    !Equal(im->FilePath,pin->file_path,pin->file_path_bytes)||
    (Which==2&&(im->DeviceHandle!=P.target||binding->ImageHandle!=P.agents[Which])))return EFI_UNSUPPORTED;
  UINTN base=(UINTN)im->ImageBase;
  if(base>~(UINTN)0-pin->image_bytes||(UINTN)binding->Supported!=base+pin->supported_offset||
    (UINTN)binding->Start!=base+pin->start_offset||(UINTN)binding->Stop!=base+pin->stop_offset||
    (!First&&(P.images[Which]!=binding->ImageHandle||P.bindings[Which]!=binding||P.loaded[Which]!=im||
      P.bases[Which]!=im->ImageBase||P.devices[Which]!=im->DeviceHandle||P.parents[Which]!=im->ParentHandle)))return EFI_NOT_READY;
  UINTN scratch_bytes=GuPeScratchBytes(pin->image_bytes);VOID *scratch=GuAlloc(scratch_bytes);
  if(!scratch)return EFI_OUT_OF_RESOURCES;
  GU_PE_IDENTITY identity;
  s=GuPeIdentity(im->ImageBase,(UINTN)im->ImageSize,base,pin->stop_offset,scratch,scratch_bytes,&identity);
  s=Free(scratch,s);if(s!=EFI_SUCCESS)return Result(s);
  if(!identity.canonical_valid||identity.executable_relocations||identity.image_bytes!=pin->image_bytes||
    identity.headers_bytes!=pin->headers_bytes||identity.entry_rva!=pin->entry_rva||
    identity.relocation_rva!=pin->relocation_rva||identity.relocation_bytes!=pin->relocation_bytes||
    identity.relocation_count!=pin->relocation_count||identity.section_count!=pin->section_count||
    !GuHashMatches((UINT8 *)im->ImageBase+identity.relocation_rva,identity.relocation_bytes,pin->relocation_sha256))return EFI_CRC_ERROR;
  UINTN executable=0;
  for(UINTN i=0;i<identity.section_count;i++) {
    GU_PE_SECTION *section=&identity.sections[i];if(!section->executable)continue;executable++;
    if(section->rva!=pin->code_rva||section->bytes!=pin->code_bytes||section->relocations||
      !Callable((UINT8 *)im->ImageBase+section->rva,section->bytes)||
      !GuHashMatches((UINT8 *)im->ImageBase+section->rva,section->bytes,pin->code_sha256))return EFI_CRC_ERROR;
    UINTN offsets[]={pin->supported_offset,pin->start_offset,pin->stop_offset};
    for(UINTN j=0;j<3;j++)if(offsets[j]<section->rva||offsets[j]-section->rva>=section->bytes)return EFI_UNSUPPORTED;
  }
  if(executable!=1)return EFI_UNSUPPORTED;
  if(First){P.images[Which]=binding->ImageHandle;P.bindings[Which]=binding;P.loaded[Which]=im;
    P.bases[Which]=im->ImageBase;P.devices[Which]=im->DeviceHandle;P.parents[Which]=im->ParentHandle;}
  if(Validated)*Validated=binding;
  return EFI_SUCCESS;
}
static EFI_STATUS Consumers(VOID) {
  UINTN pci=0,children=0,text=0,virtual=0,gfx=0,upstream=0;
  for(UINTN i=0;i<Baseline.count;i++) {
    const EDGE *e=&Baseline.edges[i];
    if(e->handle==P.target) {
      if(e->count!=1)return EFI_ACCESS_DENIED;
      if(Guid(&e->protocol,&PciGuid)&&e->attributes==0x10&&e->controller==P.target){P.agents[2]=e->agent;pci++;}
      else if(Guid(&e->protocol,&PathGuid)&&e->attributes==8&&(e->controller==P.gop||e->controller==P.edid)){children++;}
      else return EFI_ACCESS_DENIED;
    } else if(e->handle==P.gop) {
      if(e->count!=1)return EFI_ACCESS_DENIED;
      if(Guid(&e->protocol,&TextGuid)&&e->attributes==0x10&&e->controller==P.gop){P.agents[0]=e->agent;text++;}
      else if(Guid(&e->protocol,&TextGuid)&&e->attributes==8&&e->controller==P.console){virtual++;}
      else if(Guid(&e->protocol,&GopGuid)&&e->attributes==0x10&&e->controller==P.gop){P.agents[1]=e->agent;gfx++;}
      else return EFI_ACCESS_DENIED;
    } else if(e->handle==P.edid||e->handle==P.console)return EFI_ACCESS_DENIED;
  }
  if(pci!=1||children!=2||text!=1||virtual!=1||gfx!=1||!P.agents[0]||!P.agents[1]||!P.agents[2]||
    P.agents[0]==P.agents[1]||P.agents[0]==P.agents[2]||P.agents[1]==P.agents[2])return EFI_ACCESS_DENIED;
  for(UINTN d=0;d<3;d++)if(P.agents[d]==Gu.image||P.agents[d]==P.target||
    P.agents[d]==P.gop||P.agents[d]==P.edid||P.agents[d]==P.console)return EFI_ACCESS_DENIED;
  for(UINTN i=0;i<Baseline.count;i++) {
    const EDGE *e=&Baseline.edges[i];
    if(e->controller==P.target&&e->handle!=P.target) {
      if(e->handle==P.gop||e->handle==P.edid||e->handle==P.console||e->handle==Gu.image||
        !Guid(&e->protocol,&RootGuid)||e->attributes!=8||e->count!=1)return EFI_ACCESS_DENIED;
      UINTN owners=0;
      for(UINTN j=0;j<Baseline.count;j++) {
        const EDGE *owner=&Baseline.edges[j];
        if(owner->handle==e->handle&&owner->agent==e->agent&&owner->controller==e->handle&&
          Guid(&owner->protocol,&RootGuid)&&owner->attributes==16&&owner->count==1)owners++;
      }
      if(owners!=1)return EFI_ACCESS_DENIED;upstream++;
    }
    if((e->controller==P.gop||e->controller==P.edid)&&e->handle!=P.target&&e->handle!=P.gop)return EFI_ACCESS_DENIED;
    if(e->handle==P.target&&e->agent!=P.agents[2])return EFI_ACCESS_DENIED;
    if(e->handle==P.gop&&e->agent!=(Guid(&e->protocol,&TextGuid)?P.agents[0]:P.agents[1]))return EFI_ACCESS_DENIED;
    /* Each driver is dedicated to this observed output chain. */
    if((e->agent==P.agents[2]&&e->handle!=P.target)||
      ((e->agent==P.agents[0]||e->agent==P.agents[1])&&e->handle!=P.gop))return EFI_ACCESS_DENIED;
  }
  return upstream==1?EFI_SUCCESS:EFI_ACCESS_DENIED;
}
static EFI_STATUS Endpoints(VOID) {
  if(!Path(P.target,GuRehearsalPciPath,sizeof(GuRehearsalPciPath))||
    !Path(P.gop,GuRehearsalGopPath,sizeof(GuRehearsalGopPath))||
    !Path(P.edid,GuRehearsalEdidPath,sizeof(GuRehearsalEdidPath)))return EFI_NOT_READY;
  EFI_STATUS s=Inventory(P.target,GuRehearsalPciProtocols,COUNT(GuRehearsalPciProtocols));
  if(s==EFI_SUCCESS)s=Inventory(P.gop,GuRehearsalGopProtocols,COUNT(GuRehearsalGopProtocols));
  if(s==EFI_SUCCESS)s=Inventory(P.edid,GuRehearsalEdidProtocols,COUNT(GuRehearsalEdidProtocols));
  return Result(s);
}
static EFI_STATUS Paths(VOID) {
  if(!Path(P.target,GuRehearsalPciPath,sizeof(GuRehearsalPciPath)))return EFI_NOT_READY;
  if(P.stage!=3&&(!Path(P.gop,GuRehearsalGopPath,sizeof(GuRehearsalGopPath))||
    !Path(P.edid,GuRehearsalEdidPath,sizeof(GuRehearsalEdidPath))))return EFI_NOT_READY;
  /* NVIDIA may remove its private parent protocols at stage3. All captured
   * parent protocols must exist while it remains bound; EDID has no consumer. */
  EFI_STATUS s=EFI_SUCCESS;
  if(P.stage!=3)s=Inventory(P.target,GuRehearsalPciProtocols,COUNT(GuRehearsalPciProtocols));
  if(s==EFI_SUCCESS&&P.stage!=3)s=Inventory(P.edid,GuRehearsalEdidProtocols,COUNT(GuRehearsalEdidProtocols));
  return s;
}
static EFI_STATUS ConsoleCode(VOID) {
  UINTN callbacks[]={(UINTN)P.out->Reset,(UINTN)P.out->OutputString,(UINTN)P.out->SetMode,
    (UINTN)P.in->Reset,(UINTN)P.in->ReadKeyStroke};
  const GU_REHEARSAL_DRIVER_PIN *pin=&GuRehearsalDrivers[0];
  UINTN base=(UINTN)P.bases[0];
  if(COUNT(GuRehearsalConsoleOffsets)!=COUNT(callbacks))return EFI_UNSUPPORTED;
  for(UINTN i=0;i<COUNT(callbacks);i++) {
    UINTN off=GuRehearsalConsoleOffsets[i];
    if(off<pin->code_rva||off-pin->code_rva>=pin->code_bytes||callbacks[i]!=base+off||
      !Callable((VOID *)callbacks[i],1))return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}
#if GU_STAGED_UNLOCK
static BOOLEAN TransactionClean(VOID) {
  return GuConfigPostFlrClean();
}
static BOOLEAN TransactionPristine(VOID) {
  return !Gu.started&&!Gu.quiesced&&!Gu.saved&&!Gu.flrs&&!Gu.mmio_writes&&
    !Gu.fault&&!Gu.retained&&!Gu.allocations&&!Gu.dma_count&&!Gu.master_attributes_pending;
}
static EFI_STATUS StagedResult(EFI_STATUS Status) {
  Status=Result(Status);if(Status!=EFI_SUCCESS)P.staged_failed=TRUE;return Status;
}
#endif
static EFI_STATUS Verify(BOOLEAN FullEndpoints) {
  NoteUsbRetirements();
  const char *part="memory-map";EFI_STATUS s=Map();
  if(s!=EFI_SUCCESS)goto done;
  part="console";s=Console(FALSE);if(s!=EFI_SUCCESS)goto done;
  part="pci-config";s=Config(FALSE);if(s!=EFI_SUCCESS)goto done;
  part="device-paths";s=Paths();if(s!=EFI_SUCCESS)goto done;
  part="display";s=Output(FALSE);if(s!=EFI_SUCCESS)goto done;
  static const char *names[]={"text-driver","graphics-driver","nvidia-driver"};
  for(UINTN i=0;i<3;i++){part=names[i];s=Driver(i,FALSE,NULL);if(s!=EFI_SUCCESS)goto done;}
  part="console-code";s=ConsoleCode();if(s!=EFI_SUCCESS)goto done;
  if(FullEndpoints){part="endpoint-identities";s=Endpoints();if(s!=EFI_SUCCESS)goto done;}
#if GU_STAGED_UNLOCK
  if(s==EFI_SUCCESS&&P.complete_done) {
    part="transaction-state";
    if(P.staged_failed||P.claimed||Gu.owned||!P.claim_closed||!TransactionClean())s=EFI_NOT_READY;
    if(s==EFI_SUCCESS){part="unlock-post-state";s=GuStagedPostState(GuLogReady());}
  }
  if(s!=EFI_SUCCESS)goto done;
#endif
  /* All potentially logging checks precede this coherent final observation. */
  part="graph-snapshot";s=Snapshot(&Current);if(s!=EFI_SUCCESS)goto done;
  part="graph-delta";s=Expected();if(s!=EFI_SUCCESS)goto done;
  part="removed-child-handles";
  if(P.stage==3&&(HandleIn(&Current,P.original_gop)||HandleIn(&Current,P.original_edid)))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS&&Gu.fault){part="latched-fault";s=EFI_DEVICE_ERROR;}
done:
  if(s!=EFI_SUCCESS&&GuLogReady())GuLog("REHEARSAL_VERIFY_FAILED stage=%zu check=%s status=%#llx\n",
    (size_t)P.stage,part,(unsigned long long)s);
  return Result(s);
}
EFI_STATUS GuRehearsalPrepare(EFI_SYSTEM_TABLE *Table) {
  if(P.prepared||P.mutated||P.ran)return EFI_ALREADY_STARTED;
  memset(&P,0,sizeof(P));memset(&Baseline,0,sizeof(Baseline));memset(&Current,0,sizeof(Current));
  memset(UsbLeaves,0,sizeof(UsbLeaves));memset(UsbNewLeaves,0,sizeof(UsbNewLeaves));
  UsbLeafCount=0;UsbNewCount=0;UsbNewOmitted=0;UsbReplacementLogs=0;UsbFrozen=FALSE;
  if(!Table||!Table->BootServices||Table->BootServices!=Gu.bs||!Gu.image||!Gu.handle||!Gu.pci||
    Gu.started||Gu.owned||Gu.armed||Gu.fault||Gu.retained||Gu.allocations||Gu.dma_count||Gu.flrs||Gu.mmio_writes)return EFI_INVALID_PARAMETER;
  EFI_BOOT_SERVICES *b=Gu.bs;
  if(!b->GetMemoryMap||!b->HandleProtocol||!b->ProtocolsPerHandle||!b->OpenProtocolInformation||
    !b->LocateHandleBuffer||!b->LocateDevicePath||!b->AllocatePool||!b->FreePool||
    !b->DisconnectController||!b->RaiseTPL||!b->RestoreTPL)return EFI_UNSUPPORTED;
#if GU_STAGED_UNLOCK
  if(!TransactionPristine()||!b->OpenProtocol||!b->CloseProtocol||!b->RaiseTPL||!b->RestoreTPL)return EFI_UNSUPPORTED;
#endif
  P.table=Table;P.target=Gu.handle;
  EFI_STATUS s=Map();
  if(s==EFI_SUCCESS)s=Console(TRUE);
  if(s==EFI_SUCCESS)s=ResolveChildren(TRUE);
  if(s==EFI_SUCCESS)s=Endpoints();
  if(s==EFI_SUCCESS)s=Config(TRUE);
  if(s==EFI_SUCCESS)s=Output(TRUE);
  if(s==EFI_SUCCESS)s=Snapshot(&Baseline);
  if(s==EFI_SUCCESS)s=Consumers();
  for(UINTN i=0;s==EFI_SUCCESS&&i<3;i++)s=Driver(i,TRUE,NULL);
  if(s==EFI_SUCCESS)s=UsbFreeze();
  if(s==EFI_SUCCESS)s=Verify(TRUE);
  GuLog("REHEARSAL_PREPARE status=%#llx edges=%zu drivers=3 outputs=2\n",(unsigned long long)s,(size_t)Baseline.count);
  if(s==EFI_SUCCESS&&!GuLogReady())s=EFI_DEVICE_ERROR;
  if(s==EFI_SUCCESS)P.prepared=TRUE;
  return Result(s);
}
static EFI_STATUS RunStages(UINTN First,UINTN Last) {
  for(UINTN stage=First;stage<=Last;stage++) {
    GuLog("REHEARSAL_INTENT stage=%zu operation=%s\n",(size_t)stage,stage<=3?"disconnect":"connect");
    if(!GuLogReady())return EFI_DEVICE_ERROR;
    EFI_STATUS s=Verify(P.stage==0||P.stage==6);
    if(s!=EFI_SUCCESS||!GuLogReady())return Result(s)==EFI_SUCCESS?EFI_DEVICE_ERROR:Result(s);
    /* The final graph follows all filesystem logging. An error may follow a
     * partial callback: never roll back automatically or mark it unmutated. */
    P.mutated=TRUE;GuScreenDetached=TRUE;
    if(stage<=3)s=Gu.bs->DisconnectController(stage==3?P.target:P.gop,P.agents[stage-1],NULL);
    else {
      /* UEFI ConnectController's driver list only gives priority; it is not an
       * exclusive whitelist. Invoke only this freshly validated binding's
       * Supported/Start callbacks, with complete verification between them. */
      UINTN which=6-stage;EFI_HANDLE controller=stage==4?P.target:P.gop;
      EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;
      UINT32 before[64],after[64];UINT64 before_attrs=0,after_attrs=0;
      s=ExactPciState(before,&before_attrs);
      if(s==EFI_SUCCESS)s=Map();if(s==EFI_SUCCESS)s=Driver(which,FALSE,&binding);
      if(s==EFI_SUCCESS)s=binding->Supported(binding,controller,NULL);
      GuLog("REHEARSAL_SUPPORTED stage=%zu status=%#llx\n",(size_t)stage,(unsigned long long)s);
      if(s!=EFI_SUCCESS||!GuLogReady())return Result(s)==EFI_SUCCESS?EFI_DEVICE_ERROR:Result(s);
      s=ExactPciState(after,&after_attrs);
      if(s==EFI_SUCCESS&&(!Equal(before,after,sizeof(before))||before_attrs!=after_attrs)) {
        if(GuLogReady())GuLog("REHEARSAL_SUPPORTED_MUTATED_PCI stage=%zu\n",(size_t)stage);
        s=EFI_NOT_READY;
      }
      if(s==EFI_SUCCESS)s=Verify(FALSE);
      if(s==EFI_SUCCESS)s=Map();
      if(s==EFI_SUCCESS)s=Driver(which,FALSE,&binding);
      if(s!=EFI_SUCCESS||!GuLogReady())return Result(s)==EFI_SUCCESS?EFI_DEVICE_ERROR:Result(s);
      s=binding->Start(binding,controller,NULL);
    }
    GuLog("REHEARSAL_RETURN stage=%zu status=%#llx\n",(size_t)stage,(unsigned long long)s);
    if(s!=EFI_SUCCESS||!GuLogReady())return Result(s)==EFI_SUCCESS?EFI_DEVICE_ERROR:Result(s);
    P.stage=stage;
    if(stage==4) {
      s=Map();if(s==EFI_SUCCESS)s=ResolveChildren(FALSE);
      if(s!=EFI_SUCCESS)return Result(s);
    }
    s=Verify(stage==6);if(s!=EFI_SUCCESS)return Result(s);
    GuLog("REHEARSAL_STAGE_VERIFIED stage=%zu edges=%zu\n",(size_t)stage,(size_t)Current.count);
    if(!GuLogReady())return EFI_DEVICE_ERROR;
  }
  /* The caller keeps output suppressed until an additional fresh verification
   * succeeds and then owns durable logging/Windows handoff. */
  return EFI_SUCCESS;
}
EFI_STATUS GuRehearsalRun(VOID) {
#if GU_STAGED_UNLOCK
  return EFI_ACCESS_DENIED;
#else
  if(!P.prepared||P.mutated||P.ran||!GuLogReady()||Gu.fault||Gu.started||Gu.owned||
    Gu.allocations||Gu.dma_count||Gu.flrs||Gu.mmio_writes)return EFI_NOT_READY;
  P.ran=TRUE;return RunStages(1,6);
#endif
}
EFI_STATUS GuRehearsalRestored(VOID) {
#if GU_STAGED_UNLOCK
  if(!P.prepared||!P.ran||!P.mutated||P.stage!=6||P.staged_failed||!P.release_done||
    !P.claim_done||P.claimed||!P.claim_closed||!P.complete_done||!P.restore_done||
    Gu.owned||!TransactionClean())return EFI_NOT_READY;
  return StagedResult(Verify(TRUE));
#else
  if(!P.prepared||!P.ran||!P.mutated||P.stage!=6||Gu.fault||Gu.started||Gu.owned||
    Gu.allocations||Gu.dma_count||Gu.flrs||Gu.mmio_writes)return EFI_NOT_READY;
  return Result(Verify(TRUE));
#endif
}
BOOLEAN GuRehearsalMutated(VOID){return P.mutated;}

#if GU_STAGED_UNLOCK
static BOOLEAN ContainmentPci(EFI_PCI_IO_PROTOCOL *Io) {
  return Io==P.pci&&Readable(Io,sizeof(*Io))&&Equal(Io,&P.pci_api,sizeof(*Io))&&
    Callable((VOID *)(UINTN)P.pci_api.GetLocation,1)&&
    Callable((VOID *)(UINTN)P.pci_api.Pci.Read,1)&&
    Callable((VOID *)(UINTN)P.pci_api.Pci.Write,1)&&
    Callable((VOID *)(UINTN)P.pci_api.Attributes,1);
}
EFI_STATUS GuStagedRefreshPci(VOID) {
  /* A failed firmware callback may invalidate the graph or leave bus mastering
   * enabled. Containment must tolerate those states, but never guess that a
   * replacement PCI interface is safe. Publish only the original interface,
   * after fresh mapped-memory, callback and physical-identity checks. */
  EFI_PCI_IO_PROTOCOL *io=NULL;
  UINT32 config[64]={0};UINTN segment=0,bus=0,device=0,function=0;
  Gu.pci=NULL;
  if(!P.prepared||!P.mutated||!P.pci||!Gu.bs||!P.table||
    Gu.bs!=P.table->BootServices||Gu.handle!=P.target||
    !Gu.bs->GetMemoryMap||!Gu.bs->HandleProtocol)return EFI_NOT_READY;
  EFI_STATUS s=Interface(P.target,&PciGuid,(VOID **)&io);
  if(s==EFI_SUCCESS)s=Map();
  if(s!=EFI_SUCCESS)return Result(s);
  if(!ContainmentPci(io))return EFI_COMPROMISED_DATA;
  s=P.pci_api.GetLocation(io,&segment,&bus,&device,&function);
  static const UINT32 offsets[]={0,8,12,44};
  for(UINTN i=0;s==EFI_SUCCESS&&i<COUNT(offsets);i++)
    s=P.pci_api.Pci.Read(io,EfiPciIoWidthUint32,offsets[i],1,&config[offsets[i]/4]);
  if(s!=EFI_SUCCESS)return Result(s);
  if(!GuTargetConfig(config,segment,bus,device,function))return EFI_UNSUPPORTED;
  EFI_PCI_IO_PROTOCOL *after=NULL;
  s=Interface(P.target,&PciGuid,(VOID **)&after);
  if(s==EFI_SUCCESS)s=Map();if(s!=EFI_SUCCESS)return Result(s);
  if(after!=io||!ContainmentPci(io))return EFI_COMPROMISED_DATA;
  Gu.pci=io;return EFI_SUCCESS;
}
EFI_STATUS GuStagedRelease(VOID) {
  if(!P.prepared||P.mutated||P.ran||P.staged_failed||!Gu.armed||Gu.owned||
    !GuLogReady()||!TransactionPristine())return EFI_NOT_READY;
  P.ran=TRUE;Gu.display_ok=FALSE;
  EFI_STATUS s=RunStages(1,3);
  if(s==EFI_SUCCESS)P.release_done=TRUE;
  return StagedResult(s);
}
static EFI_STATUS RestoreClaimDecode(VOID) {
  UINT32 before[64],normalized[64],after[64];UINT64 attributes=0,after_attributes=0;
  EFI_STATUS s=ExactPciState(before,&attributes);if(s!=EFI_SUCCESS)return s;
  UINT32 missing=P.config[1]&~before[1]&3U;
  memcpy(normalized,before,sizeof(normalized));normalized[1]|=missing;
  UINT64 needed=(missing&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(missing&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  if(!Equal(normalized,P.config,sizeof(normalized))||attributes!=(P.attributes&~needed))return EFI_NOT_READY;
  if(needed) {
    UINT64 supported=0;
    s=P.pci->Attributes(P.pci,EfiPciIoAttributeOperationSupported,0,&supported);
    if(s!=EFI_SUCCESS)return Result(s);
    if((supported&needed)!=needed)return EFI_UNSUPPORTED;
    GuLog("STAGED_DECODE_INTENT missing=%#x attributes=%#llx\n",missing,(unsigned long long)needed);
    if(!GuLogReady()||Gu.fault)return EFI_DEVICE_ERROR;
    /* Supported and logging must not change either ownership or PCI state.
     * This API enables only bits originally present; never BUS_MASTER. */
    s=Verify(FALSE);if(s==EFI_SUCCESS)s=ExactPciState(after,&after_attributes);
    if(s!=EFI_SUCCESS)return s;
    if(!Equal(before,after,sizeof(before))||attributes!=after_attributes)return EFI_NOT_READY;
    s=P.pci->Attributes(P.pci,EfiPciIoAttributeOperationEnable,needed,NULL);
    GuLog("STAGED_DECODE_RETURN status=%#llx\n",(unsigned long long)s);
    if(s!=EFI_SUCCESS)return Result(s);
    if(!GuLogReady()||Gu.fault)return EFI_DEVICE_ERROR;
  }
  s=ExactPciState(after,&after_attributes);if(s!=EFI_SUCCESS)return s;
  if(!Equal(after,P.config,sizeof(after))||after_attributes!=P.attributes)return EFI_NOT_READY;
  P.claim_done=TRUE;
  return Verify(FALSE);
}
/* EXCLUSIVE OpenProtocol may disconnect a BY_DRIVER consumer. Prevent that
 * implicit release: after the complete graph check, mask notification callbacks
 * and recheck every target protocol for passive opens immediately before claim.
 * Only memory/protocol services run at TPL_NOTIFY; no file or console logging. */
static EFI_STATUS TargetPassive(VOID) {
  EFI_GUID **protocols=NULL;UINTN count=0,total=0,pci=0,path=0;
  EFI_STATUS s=Gu.bs->ProtocolsPerHandle(P.target,&protocols,&count);
  if(s==EFI_SUCCESS&&(!protocols||!count||count>64))s=EFI_BAD_BUFFER_SIZE;
  if(s==EFI_SUCCESS)s=Map();
  if(s==EFI_SUCCESS&&!Readable(protocols,count*sizeof(*protocols)))s=EFI_COMPROMISED_DATA;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN n=0;
    if(!Readable(protocols[i],sizeof(*protocols[i]))){s=EFI_COMPROMISED_DATA;break;}
    for(UINTN j=0;j<i;j++)if(Guid(protocols[i],protocols[j]))s=EFI_COMPROMISED_DATA;
    if(s!=EFI_SUCCESS)break;
    if(Guid(protocols[i],&PciGuid))pci++;
    if(Guid(protocols[i],&PathGuid))path++;
    s=Gu.bs->OpenProtocolInformation(P.target,protocols[i],&entries,&n);
    if(s==EFI_SUCCESS&&(n>128||(n&&!entries)||n>4096-total))s=EFI_BAD_BUFFER_SIZE;
    if(s==EFI_SUCCESS&&n)s=Map();
    if(s==EFI_SUCCESS&&n&&!Readable(entries,n*sizeof(*entries)))s=EFI_COMPROMISED_DATA;
    if(s==EFI_SUCCESS)total+=n;
    for(UINTN j=0;s==EFI_SUCCESS&&j<n;j++) {
      UINT32 a=entries[j].Attributes;
      if(!entries[j].OpenCount){s=EFI_COMPROMISED_DATA;break;}
      if(a!=EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL&&a!=EFI_OPEN_PROTOCOL_GET_PROTOCOL&&
        a!=EFI_OPEN_PROTOCOL_TEST_PROTOCOL)s=EFI_ACCESS_DENIED;
    }
    s=Free(entries,s);
  }
  if(s==EFI_SUCCESS&&(pci!=1||path!=1))s=EFI_NOT_FOUND;
  return Free(protocols,s);
}
EFI_STATUS GuStagedClaim(VOID) {
  if(!P.prepared||!P.ran||!P.release_done||P.stage!=3||P.claim_attempted||P.staged_failed||
    !Gu.armed||Gu.owned||!TransactionPristine()||!GuLogReady())return EFI_NOT_READY;
  P.claim_attempted=TRUE;Gu.display_ok=FALSE;
  GuLogText("STAGED_CLAIM_INTENT: one exclusive target PCI_IO claim; no other controller.\n");
  if(!GuLogReady()||Gu.fault)return StagedResult(EFI_DEVICE_ERROR);
  EFI_STATUS s=Verify(FALSE);if(s!=EFI_SUCCESS)return StagedResult(s);
  if(!GuLogReady())return StagedResult(EFI_DEVICE_ERROR);
  EFI_PCI_IO_PROTOCOL *io=NULL;
  EFI_TPL previous=Gu.bs->RaiseTPL(TPL_NOTIFY);
  s=TargetPassive();
  if(s==EFI_SUCCESS)s=Gu.bs->OpenProtocol(P.target,&PciGuid,(VOID **)&io,Gu.image,NULL,EFI_OPEN_PROTOCOL_EXCLUSIVE);
  if(s==EFI_SUCCESS){P.claimed=TRUE;Gu.owned=TRUE;}
  Gu.bs->RestoreTPL(previous);
  GuLog("STAGED_CLAIM_RETURN status=%#llx interface_same=%u\n",(unsigned long long)s,(unsigned)(io==P.pci));
  if(s!=EFI_SUCCESS)return StagedResult(s);
  if(io!=P.pci)return StagedResult(EFI_COMPROMISED_DATA);
  if(!GuLogReady()||Gu.fault)return StagedResult(EFI_DEVICE_ERROR);
  s=Verify(FALSE);if(s==EFI_SUCCESS)s=RestoreClaimDecode();
  if(s==EFI_SUCCESS&&!GuLogReady())s=EFI_DEVICE_ERROR;
  if(s==EFI_SUCCESS)Gu.display_ok=TRUE;
  return StagedResult(s);
}
BOOLEAN GuStagedDetached(VOID) {
  /* Also used just before FLR, with the transaction's DMA still retained and
   * secure firmware already run. Verify ownership/display/config, not a false
   * claim that no transaction has started. BME must already be contained. */
  if(!P.prepared||!P.release_done||P.stage!=3||P.staged_failed||!P.claimed||!P.claim_done||
    P.complete_attempted||!Gu.armed||!Gu.owned||Gu.fault||Gu.retained||
    Gu.master_attributes_pending||Gu.flrs||!GuLogReady())return FALSE;
  EFI_STATUS s=Verify(FALSE);
  if(s==EFI_SUCCESS&&!GuLogReady())s=EFI_DEVICE_ERROR;
  return StagedResult(s)==EFI_SUCCESS;
}
EFI_STATUS GuStagedComplete(VOID) {
  if(!P.prepared||!P.release_done||P.stage!=3||P.staged_failed||!P.claim_done||!P.claimed||
    !Gu.owned||P.complete_attempted||!GuLogReady())return EFI_NOT_READY;
  P.complete_attempted=TRUE;
  if(!TransactionClean())return StagedResult(EFI_NOT_READY);
  EFI_STATUS s=Verify(FALSE);
  if(s==EFI_SUCCESS)s=GuStagedPostState(TRUE);
  if(s!=EFI_SUCCESS)return StagedResult(s);
  GuLogText("STAGED_CLAIM_CLOSE_INTENT: verified completed transaction; closing exclusive PCI_IO.\n");
  if(!GuLogReady()||Gu.fault)return StagedResult(EFI_DEVICE_ERROR);
  s=Verify(FALSE);if(s!=EFI_SUCCESS)return StagedResult(s);
  if(!GuLogReady()||!TransactionClean())return StagedResult(EFI_DEVICE_ERROR);
  s=Gu.bs->CloseProtocol(P.target,&PciGuid,Gu.image,NULL);
  if(s==EFI_SUCCESS){P.claimed=FALSE;P.claim_closed=TRUE;Gu.owned=FALSE;Gu.display_ok=FALSE;}
  GuLog("STAGED_CLAIM_CLOSE_RETURN status=%#llx\n",(unsigned long long)s);
  if(s!=EFI_SUCCESS)return StagedResult(s);
  if(!GuLogReady()||!TransactionClean())return StagedResult(EFI_DEVICE_ERROR);
  s=Verify(FALSE);if(s==EFI_SUCCESS)s=GuStagedPostState(TRUE);
  if(s==EFI_SUCCESS&&(!GuLogReady()||!TransactionClean()))s=EFI_DEVICE_ERROR;
  if(s==EFI_SUCCESS)P.complete_done=TRUE;
  return StagedResult(s);
}
EFI_STATUS GuStagedRestore(VOID) {
  if(!P.prepared||!P.release_done||P.stage!=3||P.staged_failed||!P.complete_done||
    !P.claim_closed||P.claimed||Gu.owned||P.restore_attempted||!TransactionClean()||
    !GuLogReady())return EFI_NOT_READY;
  P.restore_attempted=TRUE;
  EFI_STATUS s=RunStages(4,6);
  if(s==EFI_SUCCESS)P.restore_done=TRUE;
  return StagedResult(s);
}
#else
EFI_STATUS GuStagedRelease(VOID){return EFI_ACCESS_DENIED;}
EFI_STATUS GuStagedClaim(VOID){return EFI_ACCESS_DENIED;}
BOOLEAN GuStagedDetached(VOID){return FALSE;}
EFI_STATUS GuStagedComplete(VOID){return EFI_ACCESS_DENIED;}
EFI_STATUS GuStagedRestore(VOID){return EFI_ACCESS_DENIED;}
EFI_STATUS GuStagedRefreshPci(VOID){return EFI_ACCESS_DENIED;}
#endif
