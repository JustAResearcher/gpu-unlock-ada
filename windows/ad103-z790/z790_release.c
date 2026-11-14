/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MSI Z790/AD103 ownership adapter.
 *
 * Adapted from windows/ad104-msi/msi_release.c. All structural logic
 * (Snapshot/Consumers/PointerBaseline/UsbBaseline/Expected/Alive/Gops/
 * ReadConfig/ConfigStable/Bar/Driver/ClaimRestore/GuZ790Release/
 * GuZ790Finish) is unchanged except for the substitutions below, every
 * one of them this board's own real capture from ownership_probe/
 * (2026-11-10, confirmed stable across two separate boots -- see
 * FINDINGS_2026-11-10.md), never copied from ad104-msi "by analogy":
 *   - PciPath/GopPath/UsbHostPath: this board's own ACPI/PCI device paths
 *     (see ../ownership_probe/README.md and the findings doc this capture
 *     produced). OutFile/GfxFile are byte-identical to ad104-msi's own --
 *     confirmed programmatically, not by eye -- generic platform UEFI
 *     reference-code module identities, not MSI- or board-specific. NvFile
 *     (NVIDIA's own GOP driver filename, "nvgop-ad1xx") also matches --
 *     that's NVIDIA's own naming, embedded in the VBIOS Option ROM.
 *   - Driver() pins (version/size/stop/code offsets, and every hash): this
 *     board's own captured driver identities. Unlike ad104-msi's apparent
 *     hand-picked code ranges (its own "CAP0004" comment doesn't say how
 *     those were chosen), the hashed code range here is whichever section
 *     this board's own loaded driver's PE headers mark as code -- a
 *     reproducible, checkable choice, not a manual one.
 *   - Bar()'s AddrRangeMax: GU_BAR0_BASE + 0xFFFFFF, recomputed for this
 *     board's own BAR0_BASE (0x81000000), not reused from ad104-msi's
 *     literal (which was for BAR0_BASE=0x80000000).
 *   - GuZ790ReadyForWindows()'s chip id: 0x193000a1 (AD103), matching
 *     profile.c's own pinned value for offset 0 -- not ad104-msi's AD104
 *     value 0x194000a1.
 *   - GuAd104ColdEntry() -> GuAd103ColdEntry() throughout.
 *
 * STATUS: complete. This is the first file in the port that mutates
 * anything -- GuZ790Release() calls DisconnectController three times (the
 * console-out driver, then the GOP driver, then the GPU's own PCI_IO/
 * NVIDIA driver) and then, via ClaimRestore(), opens the GPU's PCI_IO
 * protocol EFI_OPEN_PROTOCOL_EXCLUSIVE and enables whatever I/O/memory
 * decode bits were left disabled; GuZ790Finish() calls CloseProtocol to
 * release that claim afterwards. Both re-verify the full protocol graph
 * and this board's cold-entry state between every step and refuse on any
 * mismatch (see Expected()/GuAd103ColdEntry()/ConfigStable() throughout).
 * Still true of everything here: no PCI config-space write, no BAR0 write,
 * no FLR -- those live in port.c/vendor/unlock_610.c, gated behind
 * Gu.armed/Gu.owned/Gu.profile_verified, and are reached only from app.c,
 * only after a human presses "U" at GuBootChoose()'s prompt.
 */
#include "z790_release.h"
#include "boot_io.h"
#include "profile.h"
#include "sha256.h"
#include "compat/compat.h"
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <IndustryStandard/Acpi10.h>

#define MAX_EDGES 4096
typedef struct {EFI_HANDLE handle,agent,controller;EFI_GUID protocol;UINT32 attributes,count;} EDGE;
typedef struct {UINTN count;EDGE edges[MAX_EDGES];} GRAPH;
typedef struct {
  EFI_SYSTEM_TABLE *table;EFI_HANDLE self,pci,gop,out_agent,gfx_agent,nv_agent;
  EFI_HANDLE console,input,error,pointer_agent,key_agent,usb_host,usb_agent;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *text,*err;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *keys;
  UINTN key_reset,key_read;EFI_EVENT key_wait;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop_interface;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
  UINT32 config[64];UINT64 attributes;
  UINTN stage,reported_additions,reported_removals;BOOLEAN ready,mutated,claimed;
} PLAN;
static PLAN Plan;
static GRAPH Original,Current;
static BOOLEAN OriginalUsb[MAX_EDGES];
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,TextGuid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
static EFI_GUID InputGuid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
/* Generic UEFI spec GUIDs, no vendored named macro for either -- same raw
 * literals ad104-msi/msi_release.c itself uses. */
static EFI_GUID PointerGuid={0x31878c87,0x0b75,0x11d5,{0x9a,0x4f,0,0x90,0x27,0x3f,0xc1,0x4d}};
static EFI_GUID UsbHcGuid={0x3e745226,0x9818,0x45b6,{0xa2,0xac,0xd7,0xcd,0x0e,0x8b,0xa2,0xbc}};
/* This board's own captured USB host-controller device path (ownership_probe/,
 * 2026-11-10): ACPI(hid=0x0a0341d0,uid=0) / PCI(device=0x14,function=0). PCI
 * 0:14.0 is the standard location for an Intel PCH's built-in xHCI controller. */
static const UINT8 UsbHostPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,0x14,127,255,4,0};
static EFI_GUID BindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID,ImageGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
/* This board's own captured GPU PCI_IO device path: ACPI(hid=0x0a0341d0,uid=0)
 * / PCI(1,0) / PCI(0,0) -- one PCIe bridge at device 1, the GPU function
 * itself at device 0. Neither this nor GopPath below matches ad104-msi's
 * exact node values; that board's topology is a different machine entirely. */
static const UINT8 PciPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,1,1,1,6,0,0,0,127,255,4,0};
/* Same prefix as PciPath, plus one more node: ACPI_ADR(0x80010340) -- the
 * physical GOP handle's own path, captured the same run. */
static const UINT8 GopPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,1,1,1,6,0,0,0,2,3,8,0,0x40,3,1,0x80,127,255,4,0};
/* OutFile/GfxFile: byte-identical to ad104-msi's own constants, confirmed
 * programmatically against this board's capture -- generic platform UEFI
 * reference-code module identities (the console-output and GOP-arbitration
 * drivers EDK2/OEM firmware ships), not MSI- or board-specific. */
static const UINT8 OutFile[]={4,6,20,0,0x7d,0x49,0x8a,0x62,0xf6,0x2b,0x64,0x42,0x87,0x41,6,0x9d,0xbd,0x33,0x99,0xd6,127,255,4,0};
static const UINT8 GfxFile[]={4,6,20,0,0xdd,0xab,0xe7,0x43,0x52,0xe3,0xfb,0x4c,0xa2,0x30,0x4c,0xdc,0x1d,0x35,0x0e,0x5c,127,255,4,0};
/* NvFile: also byte-identical to ad104-msi's own -- NVIDIA's own GOP driver
 * filename ("nvgop-ad1xx"), embedded in the VBIOS Option ROM, same across
 * Ada-generation cards regardless of board. */
static const UINT8 NvFile[]={4,4,28,0,'n',0,'v',0,'g',0,'o',0,'p',0,'-',0,'a',0,'d',0,'1',0,'x',0,'x',0,0,0,127,255,4,0};

static BOOLEAN Equal(const VOID *a,const VOID *b,UINTN n){return !memcmp(a,b,n);}
static BOOLEAN Guid(const EFI_GUID *a,const EFI_GUID *b){return Equal(a,b,16);}
static EFI_STATUS Interface(EFI_HANDLE h,EFI_GUID *g,VOID **p) {
  *p=NULL;EFI_STATUS s=Plan.table->BootServices->HandleProtocol(h,g,p);
  return s==EFI_SUCCESS&&!*p?EFI_COMPROMISED_DATA:s;
}
/* Match exact, bounded, single-instance paths; never accept a prefix alone. */
static BOOLEAN Path(EFI_HANDLE h,const UINT8 *bytes,UINTN count) {
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS)return FALSE;
  UINTN at=0;
  while(at+4<=count) {
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||n>count-at||!Equal(p,bytes+at,n))return FALSE;
    if(p->Type==END_DEVICE_PATH_TYPE)return n==4&&p->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE&&at+4==count;
    at+=n;p=(VOID *)((UINT8 *)p+n);
  }
  return FALSE;
}
static EFI_STATUS Snapshot(GRAPH *g) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_HANDLE *handles=NULL;UINTN count=0;g->count=0;
  EFI_STATUS s=b->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&handles);
  if(s==EFI_SUCCESS&&(!handles||!count||count>4096))s=EFI_BAD_BUFFER_SIZE;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GUID **protocols=NULL;UINTN pc=0;
    s=b->ProtocolsPerHandle(handles[i],&protocols,&pc);
    if(s==EFI_SUCCESS&&(!protocols||!pc||pc>64))s=EFI_BAD_BUFFER_SIZE;
    for(UINTN j=0;s==EFI_SUCCESS&&j<pc;j++) {
      EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN ec=0;
      if(!protocols[j]){s=EFI_COMPROMISED_DATA;break;}
      s=b->OpenProtocolInformation(handles[i],protocols[j],&entries,&ec);
      if(s==EFI_SUCCESS&&(ec>128||(ec&&!entries)))s=EFI_BAD_BUFFER_SIZE;
      for(UINTN k=0;s==EFI_SUCCESS&&k<ec;k++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[k];
        if(e->Attributes==1||e->Attributes==2||e->Attributes==4)continue;
        BOOLEAN own=Plan.claimed&&handles[i]==Plan.pci&&e->AgentHandle==Plan.self&&
          !e->ControllerHandle&&e->Attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->OpenCount==1;
        if(g->count==MAX_EDGES||!e->AgentHandle||(!e->ControllerHandle&&!own)||!e->OpenCount){s=EFI_COMPROMISED_DATA;break;}
        g->edges[g->count++]=(EDGE){handles[i],e->AgentHandle,e->ControllerHandle,*protocols[j],e->Attributes,e->OpenCount};
      }
      if(entries&&b->FreePool(entries)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
    }
    if(protocols&&b->FreePool(protocols)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  }
  if(handles&&b->FreePool(handles)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  return s;
}
static BOOLEAN SameEdge(const EDGE *a,const EDGE *b) {
  return a->handle==b->handle&&a->agent==b->agent&&a->controller==b->controller&&
    a->attributes==b->attributes&&a->count==b->count&&Guid(&a->protocol,&b->protocol);
}
static BOOLEAN Removed(const EDGE *e,UINTN stage) {
  if(stage>=1&&e->handle==Plan.gop&&e->agent==Plan.out_agent&&Guid(&e->protocol,&TextGuid))return TRUE;
  if(stage>=2&&e->handle==Plan.gop&&e->agent==Plan.gfx_agent&&Guid(&e->protocol,&GopGuid))return TRUE;
  return stage>=3&&e->handle==Plan.pci&&e->agent==Plan.nv_agent;
}
static BOOLEAN ProtectedHandle(EFI_HANDLE h) {
  return h&&(h==Plan.pci||h==Plan.gop||h==Plan.self||h==Plan.out_agent||
    h==Plan.gfx_agent||h==Plan.nv_agent||h==Plan.console||h==Plan.input||h==Plan.error);
}
static BOOLEAN ProtectedEdge(const EDGE *e) {
  return ProtectedHandle(e->handle)||ProtectedHandle(e->agent)||ProtectedHandle(e->controller);
}
static BOOLEAN OwnEdge(const EDGE *e) {
  return e->handle==Plan.pci&&e->agent==Plan.self&&!e->controller&&
    Guid(&e->protocol,&PciGuid)&&e->attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->count==1;
}
static BOOLEAN KnownEdge(const EDGE *e,UINTN stage) {
  for(UINTN i=0;i<Original.count;i++)
    if(!Removed(&Original.edges[i],stage)&&SameEdge(&Original.edges[i],e))return TRUE;
  return FALSE;
}
static BOOLEAN UsbPointerPath(EFI_HANDLE h) {
  static const UINT8 host[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,0x14};
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS||!Equal(p,host,sizeof(host)))return FALSE;
  UINT8 *at=(UINT8 *)p+sizeof(host);
  for(UINTN depth=0;depth<8;depth++) {
    if(at[0]!=MESSAGING_DEVICE_PATH||at[1]!=MSG_USB_DP||at[2]!=6||at[3]!=0)return FALSE;
    at+=6;
    if(at[0]==END_DEVICE_PATH_TYPE)return at[1]==END_ENTIRE_DEVICE_PATH_SUBTYPE&&at[2]==4&&at[3]==0;
  }
  return FALSE;
}
static BOOLEAN PointerOwner(const GRAPH *g,const EDGE *e) {
  UINTN owners=0;
  for(UINTN i=0;i<g->count;i++) {
    const EDGE *o=&g->edges[i];
    if(o->handle==e->handle&&Guid(&o->protocol,&e->protocol)&&
       o->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER) {
      if(o->agent!=e->agent||o->controller!=e->handle||o->count!=1)return FALSE;
      owners++;
    }
  }
  return owners==1;
}
static BOOLEAN PointerAddition(const EDGE *e) {
  BOOLEAN splitter=(Guid(&e->protocol,&PointerGuid)&&Plan.pointer_agent&&e->agent==Plan.pointer_agent)||
    (Guid(&e->protocol,&InputGuid)&&Plan.key_agent&&e->agent==Plan.key_agent);
  return splitter&&e->controller==Plan.console&&
    !ProtectedHandle(e->handle)&&!ProtectedHandle(e->agent)&&
    e->attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER&&e->count==1&&
    PointerOwner(&Current,e)&&UsbPointerPath(e->handle);
}
static EFI_STATUS PointerBaseline(VOID) {
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    BOOLEAN pointer=Guid(&e->protocol,&PointerGuid),key=Guid(&e->protocol,&InputGuid);
    if((!pointer&&!key)||e->controller!=Plan.console||e->attributes!=8)continue;
    EFI_HANDLE *agent=pointer?&Plan.pointer_agent:&Plan.key_agent;
    if(e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent)||
       !PointerOwner(&Original,e)||!UsbPointerPath(e->handle)||
       (*agent&&e->agent!=*agent))return EFI_UNSUPPORTED;
    *agent=e->agent;
  }
  return EFI_SUCCESS;
}
static BOOLEAN UsbConsoleChild(const EDGE *e,const GRAPH *g) {
  BOOLEAN splitter=(Guid(&e->protocol,&PointerGuid)&&Plan.pointer_agent&&e->agent==Plan.pointer_agent)||
    (Guid(&e->protocol,&InputGuid)&&Plan.key_agent&&e->agent==Plan.key_agent);
  return splitter&&e->controller==Plan.console&&e->attributes==8&&e->count==1&&PointerOwner(g,e);
}
static EFI_STATUS UsbBaseline(VOID) {
  memset(OriginalUsb,0,sizeof(OriginalUsb));
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(!Guid(&e->protocol,&UsbHcGuid)||e->attributes!=0x10||e->controller!=e->handle||
       e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent)||
       !Path(e->handle,UsbHostPath,sizeof(UsbHostPath)))continue;
    if(Plan.usb_host)return EFI_UNSUPPORTED;
    Plan.usb_host=e->handle;Plan.usb_agent=e->agent;
  }
  if(!Plan.usb_host||!Plan.usb_agent)return EFI_UNSUPPORTED;
  UINTN dynamic=0;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->count!=1||ProtectedHandle(e->handle)||ProtectedHandle(e->agent))continue;
    BOOLEAN child=e->handle==Plan.usb_host&&e->agent==Plan.usb_agent&&
      Guid(&e->protocol,&UsbHcGuid)&&e->attributes==8&&!ProtectedHandle(e->controller)&&
      UsbPointerPath(e->controller);
    BOOLEAN physical=UsbPointerPath(e->handle);
    BOOLEAN device=physical&&((e->attributes==0x10&&e->controller==e->handle)||
      (e->attributes==8&&!ProtectedHandle(e->controller)&&UsbPointerPath(e->controller))||
      UsbConsoleChild(e,&Original));
    if(child||device){OriginalUsb[i]=TRUE;dynamic++;}
  }
  GuLog("Z790 USB baseline: host=%#llx bus_agent=%#llx dynamic_edges=%zu; host ownership pinned\n",
    (unsigned long long)(UINTN)Plan.usb_host,(unsigned long long)(UINTN)Plan.usb_agent,(size_t)dynamic);
  return EFI_SUCCESS;
}
static BOOLEAN UsbRemoved(UINTN i) {
  if(!OriginalUsb[i])return FALSE;
  const EDGE *e=&Original.edges[i];
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *c=&Current.edges[j];
    if(c->handle==e->handle&&c->agent==e->agent&&c->controller==e->controller&&Guid(&c->protocol,&e->protocol))return FALSE;
  }
  return TRUE;
}
static BOOLEAN Expected(UINTN stage,BOOLEAN claimed) {
  UINTN own=0;
  for(UINTN i=0;i<Original.count;i++) {
    if(Removed(&Original.edges[i],stage))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&Original.edges[i],&Current.edges[j]))matches++;
    if(matches!=1&&!(matches==0&&UsbRemoved(i)))return FALSE;
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(OwnEdge(e)){own++;continue;}
    if(!KnownEdge(e,stage)&&ProtectedEdge(e)&&!PointerAddition(e))return FALSE;
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k]))return FALSE;
  }
  return own==(claimed?1u:0u);
}
#define NOTE(...) do{if(GuBootLogOpen())GuLog(__VA_ARGS__);}while(0)
static VOID NoteEdge(const char *kind,const EDGE *e) {
  NOTE("Z790 graph %s: handle=%#llx agent=%#llx controller=%#llx protocol=%#x attributes=%#x count=%u\n",kind,
    (unsigned long long)(UINTN)e->handle,(unsigned long long)(UINTN)e->agent,(unsigned long long)(UINTN)e->controller,
    e->protocol.Data1,e->attributes,e->count);
}
static VOID GraphDiff(BOOLEAN claimed) {
  for(UINTN i=0;i<Original.count;i++) {
    if(Removed(&Original.edges[i],3))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&Original.edges[i],&Current.edges[j]))matches++;
    if(matches!=1&&!(matches==0&&UsbRemoved(i))){NoteEdge(matches?"duplicate":"missing",&Original.edges[i]);return;}
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(!KnownEdge(e,3)&&!(claimed&&OwnEdge(e))&&ProtectedEdge(e)&&!PointerAddition(e)){NoteEdge("unexpected",e);return;}
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k])){NoteEdge("duplicate",e);return;}
  }
}
static VOID ReportAdditions(VOID) {
  UINTN added=0,removed=0;
  for(UINTN i=0;i<Original.count;i++)if(UsbRemoved(i))removed++;
  if(removed!=Plan.reported_removals&&GuBootLogOpen()) {
    NOTE("Z790 graph: %zu original USB relationships absent; host/GPU ownership and console callbacks unchanged\n",(size_t)removed);
    Plan.reported_removals=removed;
  }
  for(UINTN j=0;j<Current.count;j++)
    if(!OwnEdge(&Current.edges[j])&&!KnownEdge(&Current.edges[j],3))added++;
  if(added&&added!=Plan.reported_additions&&GuBootLogOpen()) {
    NOTE("Z790 graph: accepted %zu new edges (unrelated or verified USB pointer); GPU ownership unchanged\n",(size_t)added);
    for(UINTN j=0,shown=0;j<Current.count&&shown<8;j++)
      if(!OwnEdge(&Current.edges[j])&&!KnownEdge(&Current.edges[j],3)){NoteEdge("new-unrelated",&Current.edges[j]);shown++;}
    Plan.reported_additions=added;
  }
}
/* which: 0=out, 1=gfx, 2=nv. Every size/stop/code/bytes/hash/version value
 * below is this board's own capture from ownership_probe/ (2026-11-10), not
 * ad104-msi's. The hashed code range is this board's own PE-header-identified
 * .text section for each driver, not a hand-picked offset. */
static EFI_STATUS Driver(UINTN which) {
  EFI_HANDLE a=which==0?Plan.out_agent:which==1?Plan.gfx_agent:Plan.nv_agent;
  EFI_DRIVER_BINDING_PROTOCOL *d=NULL;EFI_LOADED_IMAGE_PROTOCOL *im=NULL;
  EFI_STATUS s=Interface(a,&BindingGuid,(VOID **)&d);
  UINT32 version=which==0?0x10u:which==1?0x10u:0x70011u;
  if(s!=EFI_SUCCESS||!d->Stop||d->DriverBindingHandle!=a||!d->ImageHandle||
     d->Version!=version)return EFI_UNSUPPORTED;
  s=Interface(d->ImageHandle,&ImageGuid,(VOID **)&im);if(s!=EFI_SUCCESS)return s;
  UINTN size=which==0?25120:which==1?15328:162496;
  UINTN stop=which==0?0x1390:which==1?0xb5c:0x6ec;
  UINTN code=which==0?0x2a0:which==1?0x2a0:0x280;
  UINTN bytes=which==0?0x4fa0:which==1?0x1e20:0x21bc7;
  const char *hash=which==0?"0febfc8415226067e906adecb896ab45b03e969eac360a01ccfe81dd04c8a47e":
    which==1?"ece28d4e20f28f5ca16bbe0a49ad6b0ad33956d55851c4ed620f617a6c237097":
    "216c7270fa08ad9e4c3694d34d825e9e904554a1af5ffa2aeead4317c5537b37";
  const UINT8 *file=which==0?OutFile:which==1?GfxFile:NvFile;
  UINTN file_bytes=which==0?sizeof(OutFile):which==1?sizeof(GfxFile):sizeof(NvFile);
  if(!im->ImageBase||im->ImageSize!=size||im->ImageCodeType!=EfiBootServicesCode||
     (UINTN)d->Stop!=(UINTN)im->ImageBase+stop||!im->FilePath||
     !Equal(im->FilePath,file,file_bytes)||code+bytes>size||
     (which==2&&(im->DeviceHandle!=Plan.pci||d->ImageHandle!=a)))return EFI_UNSUPPORTED;
  return GuHashMatches((UINT8 *)im->ImageBase+code,bytes,hash)?EFI_SUCCESS:EFI_CRC_ERROR;
}
static EFI_STATUS Alive(VOID) {
  EFI_SYSTEM_TABLE *t=Plan.table;VOID *in=NULL,*out=NULL,*err=NULL;
  if(t->ConsoleOutHandle!=Plan.console||t->ConsoleInHandle!=Plan.input||t->StandardErrorHandle!=Plan.error||
     t->ConIn!=Plan.keys||t->ConOut!=Plan.text||t->StdErr!=Plan.err||!t->ConIn||
     (UINTN)t->ConIn->Reset!=Plan.key_reset||(UINTN)t->ConIn->ReadKeyStroke!=Plan.key_read||
     t->ConIn->WaitForKey!=Plan.key_wait)return EFI_NOT_READY;
  EFI_STATUS s=Interface(Plan.input,&InputGuid,&in);
  if(s==EFI_SUCCESS)s=Interface(Plan.console,&TextGuid,&out);
  if(s==EFI_SUCCESS)s=Interface(Plan.error,&TextGuid,&err);
  return s==EFI_SUCCESS&&(in!=Plan.keys||out!=Plan.text||err!=Plan.err)?EFI_NOT_READY:s;
}
static EFI_STATUS Gops(BOOLEAN first) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_HANDLE *h=NULL;UINTN count=0,physical=0;
  EFI_STATUS s=b->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&h);
  if(Plan.stage>=3&&s==EFI_NOT_FOUND)return EFI_SUCCESS;
  if(s==EFI_SUCCESS&&(count>2||(count&&!h)))s=EFI_UNSUPPORTED;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;s=Interface(h[i],&GopGuid,(VOID **)&g);if(s!=EFI_SUCCESS)break;
    if(h[i]==Plan.console&&g->Mode&&!g->Mode->FrameBufferBase&&!g->Mode->FrameBufferSize)continue;
    if(Plan.stage>=3||!Path(h[i],GopPath,sizeof(GopPath))||++physical!=1||!g->Mode||!g->Mode->Info||
       g->Mode->SizeOfInfo<sizeof(*g->Mode->Info)||!g->Mode->FrameBufferBase||!g->Mode->FrameBufferSize){s=EFI_UNSUPPORTED;break;}
    if(first) {Plan.gop=h[i];Plan.gop_interface=g;Plan.mode=*g->Mode;Plan.info=*g->Mode->Info;}
    else if(h[i]!=Plan.gop||g!=Plan.gop_interface||!Equal(g->Mode,&Plan.mode,sizeof(Plan.mode))||!Equal(g->Mode->Info,&Plan.info,sizeof(Plan.info)))s=EFI_NOT_READY;
  }
  if(h&&b->FreePool(h)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  if(s==EFI_SUCCESS&&physical!=(Plan.stage>=3?0u:1u))s=EFI_NOT_READY;
  return s;
}
static EFI_STATUS ReadConfig(UINT32 c[64]) {
  EFI_PCI_IO_PROTOCOL *io=NULL;UINTN seg,bus,dev,fun;
  EFI_STATUS s=Interface(Plan.pci,&PciGuid,(VOID **)&io);
  if(s!=EFI_SUCCESS||io!=Gu.pci||!io->Pci.Read||!io->GetLocation)return EFI_NOT_READY;
  s=io->GetLocation(io,&seg,&bus,&dev,&fun);
  if(s!=EFI_SUCCESS||seg||bus!=1||dev||fun)return EFI_UNSUPPORTED;
  s=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,c);
  if(s==EFI_SUCCESS&&(!GuTargetConfig(c,seg,bus,dev,fun)||(c[1]&4)))s=EFI_UNSUPPORTED;
  return s;
}
static BOOLEAN ConfigStable(const UINT32 c[64]) {
  for(UINTN i=0;i<64;i++) {
    UINT32 drift=c[i]^Plan.config[i];
    if(!drift)continue;
    BOOLEAN allowed=Gu.flrs==1&&i==0x80/4&&Plan.config[0x78/4]==0x0012b410&&
      !(c[i]&~Plan.config[i])&&!(drift&~0x00090000u);
    if(!allowed) {
      NOTE("PCI config drift refused: offset=%#x captured=%#x now=%#x bits=%#x flrs=%u\n",
        (unsigned)(i*4),Plan.config[i],c[i],drift,Gu.flrs);
      return FALSE;
    }
    NOTE("PCI status drift tolerated: offset=%#x captured=%#x now=%#x bits=%#x flrs=%u\n",
      (unsigned)(i*4),Plan.config[i],c[i],drift,Gu.flrs);
  }
  return TRUE;
}
/* AddrRangeMax = GU_BAR0_BASE+0xFFFFFF, recomputed for this board's own
 * BAR0_BASE (0x81000000 -> 0x81FFFFFF), not ad104-msi's literal
 * (0x80000000 -> 0x80FFFFFF). */
static EFI_STATUS Bar(VOID) {
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=NULL;UINT64 support=0;
  if(!Gu.pci->GetBarAttributes)return EFI_UNSUPPORTED;
  EFI_STATUS s=Gu.pci->GetBarAttributes(Gu.pci,0,&support,(VOID **)&d);
  if(s==EFI_SUCCESS&&(!d||d->Desc!=0x8a||d->Len!=43||d->ResType||
     d->AddrRangeMin!=GU_BAR0_BASE||d->AddrRangeMax!=0x81ffffffULL||
     d->AddrTranslationOffset||d->AddrLen!=0x1000000))s=EFI_UNSUPPORTED;
  if(d&&Plan.table->BootServices->FreePool(d)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  return s;
}
static EFI_STATUS Consumers(VOID) {
  UINTN pci=0,child=0,text=0,virtual=0,gfx=0;
  EFI_HANDLE child_handles[2]={0,0};
  GuLog("Consumers: pci=%#llx gop=%#llx console=%#llx (added 2026-11-10: diagnostic only)\n",
    (unsigned long long)(UINTN)Plan.pci,(unsigned long long)(UINTN)Plan.gop,(unsigned long long)(UINTN)Plan.console);
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->handle==Plan.pci) {
      if(e->count!=1) {
        GuLog("Consumers: FAIL pci-edge count!=1 agent=%#llx protocol=%#x attributes=%#x count=%u\n",
          (unsigned long long)(UINTN)e->agent,e->protocol.Data1,e->attributes,e->count);
        return EFI_ACCESS_DENIED;
      }
      if(Guid(&e->protocol,&PciGuid)&&e->attributes==0x10&&e->controller==Plan.pci){Plan.nv_agent=e->agent;pci++;}
      else if(Guid(&e->protocol,&PathGuid)&&e->attributes==8){
        /* 2026-11-10: don't require controller==Plan.gop here -- this
         * board's PCI handle has two device-path children (see the file
         * header comment and FINDINGS_2026-11-10.md), and requiring the
         * pinned GOP handle specifically made the second, confirmed-inert
         * child always fail classification even though the tally check
         * below already tolerates child==2. Just count distinct children
         * here; the block right after the tally check verifies one of
         * them really is Plan.gop and, if there's a second, that it is
         * the same otherwise-unreferenced handle the probe found. */
        if(child<2)child_handles[child]=e->controller;
        child++;
      }
      else {
        GuLog("Consumers: FAIL unclassified pci-edge agent=%#llx controller=%#llx protocol=%#x attributes=%#x count=%u\n",
          (unsigned long long)(UINTN)e->agent,(unsigned long long)(UINTN)e->controller,e->protocol.Data1,e->attributes,e->count);
        return EFI_ACCESS_DENIED;
      }
    } else if(e->handle==Plan.gop) {
      if(e->count!=1) {
        GuLog("Consumers: FAIL gop-edge count!=1 agent=%#llx protocol=%#x attributes=%#x count=%u\n",
          (unsigned long long)(UINTN)e->agent,e->protocol.Data1,e->attributes,e->count);
        return EFI_ACCESS_DENIED;
      }
      if(Guid(&e->protocol,&TextGuid)&&e->attributes==0x10&&e->controller==Plan.gop){Plan.out_agent=e->agent;text++;}
      else if(Guid(&e->protocol,&TextGuid)&&e->attributes==8&&e->controller==Plan.console){virtual++;}
      else if(Guid(&e->protocol,&GopGuid)&&e->attributes==0x10&&e->controller==Plan.gop){Plan.gfx_agent=e->agent;gfx++;}
      else {
        GuLog("Consumers: FAIL unclassified gop-edge agent=%#llx controller=%#llx protocol=%#x attributes=%#x count=%u\n",
          (unsigned long long)(UINTN)e->agent,(unsigned long long)(UINTN)e->controller,e->protocol.Data1,e->attributes,e->count);
        return EFI_ACCESS_DENIED;
      }
    }
  }
  GuLog("Consumers: tallies pci=%u child=%u text=%u virtual=%u gfx=%u nv_agent=%#llx out_agent=%#llx gfx_agent=%#llx\n",
    (unsigned)pci,(unsigned)child,(unsigned)text,(unsigned)virtual,(unsigned)gfx,
    (unsigned long long)(UINTN)Plan.nv_agent,(unsigned long long)(UINTN)Plan.out_agent,(unsigned long long)(UINTN)Plan.gfx_agent);
  /* ad104-msi requires child==1 here. This board's own capture shows two
   * device-path children on the GPU's PCI handle: the physical GOP handle
   * (the one Plan.gop is pinned to by Gops() above) and a second,
   * otherwise-unreferenced handle with no protocols of its own anywhere in
   * the graph -- not the Intel iGPU (no second PCI_IO-bound handle exists
   * anywhere in the capture at all; both child edges are opened by the
   * same nv_agent). Most likely a second physical display connector on the
   * card with nothing attached. Confirmed byte-for-byte stable across two
   * separate boots (2026-11-10) -- see FINDINGS_2026-11-10.md -- so this is
   * deliberately `child==1 || child==2`, not silently widened to "any
   * count": 1 would be ad104-msi's own assumption, 2 is this board's own
   * confirmed reality. */
  if(pci!=1||(child!=1&&child!=2)||text!=1||virtual!=1||gfx!=1||Plan.nv_agent==Plan.out_agent||
     Plan.out_agent==Plan.gfx_agent||Plan.gfx_agent==Plan.nv_agent) {
    GuLog("Consumers: FAIL tally check (see tallies line above)\n");
    return EFI_ACCESS_DENIED;
  }
  {
    BOOLEAN saw_gop=FALSE;EFI_HANDLE extra=0;
    for(UINTN k=0;k<child;k++){
      if(child_handles[k]==Plan.gop)saw_gop=TRUE;
      else extra=child_handles[k];
    }
    if(!saw_gop) {
      GuLog("Consumers: FAIL child set does not include Plan.gop=%#llx\n",(unsigned long long)(UINTN)Plan.gop);
      return EFI_ACCESS_DENIED;
    }
    if(child==2&&child_handles[0]==child_handles[1]) {
      /* 2026-11-10, review finding: two edges naming the *same* controller
       * (both ==Plan.gop) would otherwise leave `extra` at its 0
       * initializer and vacuously pass the inertness scan below. Require
       * the two recorded children be genuinely distinct handles. */
      GuLog("Consumers: FAIL duplicate child controller=%#llx (expected two distinct handles)\n",
        (unsigned long long)(UINTN)child_handles[0]);
      return EFI_ACCESS_DENIED;
    }
    if(child==2) {
      /* 2026-11-10, review finding: checking handle==extra alone only
       * proves `extra` publishes no protocols of its own -- it misses
       * `extra` showing up as some OTHER edge's agent (meaning it's a
       * driver managing something, not a leaf) or as some OTHER edge's
       * controller (meaning it's a parent of further children). Reject
       * both; the one controller==extra edge this loop must still allow
       * is the very child-edge above that named `extra` in the first
       * place (handle==Plan.pci, PathGuid, attributes==8). */
      for(UINTN i2=0;i2<Original.count;i2++) {
        const EDGE *e2=&Original.edges[i2];
        BOOLEAN is_its_own_child_edge=(e2->handle==Plan.pci&&Guid(&e2->protocol,&PathGuid)&&
          e2->attributes==8&&e2->controller==extra);
        if(e2->handle==extra||e2->agent==extra||(e2->controller==extra&&!is_its_own_child_edge)) {
          GuLog("Consumers: FAIL second child handle=%#llx has its own graph entries (not inert)\n",
            (unsigned long long)(UINTN)extra);
          return EFI_ACCESS_DENIED;
        }
      }
    }
  }
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->handle==Plan.pci&&e->agent!=Plan.nv_agent) {
      GuLog("Consumers: FAIL second pci pass agent=%#llx!=nv_agent=%#llx\n",
        (unsigned long long)(UINTN)e->agent,(unsigned long long)(UINTN)Plan.nv_agent);
      return EFI_ACCESS_DENIED;
    }
    if(e->handle==Plan.gop&&Guid(&e->protocol,&TextGuid)&&e->agent!=Plan.out_agent) {
      GuLog("Consumers: FAIL second gop pass agent=%#llx!=out_agent=%#llx\n",
        (unsigned long long)(UINTN)e->agent,(unsigned long long)(UINTN)Plan.out_agent);
      return EFI_ACCESS_DENIED;
    }
  }
  for(UINTN i=0;i<3;i++){EFI_STATUS s=Driver(i);if(s!=EFI_SUCCESS){GuLog("Consumers: FAIL Driver(%u)=%#llx\n",(unsigned)i,(unsigned long long)s);return s;}}
  return EFI_SUCCESS;
}
EFI_STATUS GuZ790Preflight(EFI_SYSTEM_TABLE *t,EFI_HANDLE self,EFI_HANDLE pci) {
  if(Plan.mutated||Plan.ready)return EFI_ALREADY_STARTED;
  memset(&Plan,0,sizeof(Plan));Plan.table=t;Plan.self=self;Plan.pci=pci;
  if(!t||!t->BootServices||!self||!pci||Gu.handle!=pci||Gu.image!=self||Gu.bs!=t->BootServices||
     !Gu.profile_verified||!Gu.prom_verified||Gu.started||Gu.owned||Gu.armed)return EFI_INVALID_PARAMETER;
  EFI_BOOT_SERVICES *b=t->BootServices;
  if(!b->DisconnectController||!b->OpenProtocol||!b->CloseProtocol||!b->RaiseTPL||!b->RestoreTPL||
     !b->ProtocolsPerHandle||!b->OpenProtocolInformation||!b->LocateHandleBuffer||!b->HandleProtocol)return EFI_UNSUPPORTED;
  Plan.console=t->ConsoleOutHandle;Plan.input=t->ConsoleInHandle;Plan.error=t->StandardErrorHandle;
  Plan.text=t->ConOut;Plan.err=t->StdErr;Plan.keys=t->ConIn;
  if(!Plan.console||Plan.console!=Plan.input||Plan.console!=Plan.error||!Plan.text||!Plan.err||!Plan.keys||
     !Plan.keys->ReadKeyStroke||!Plan.keys->Reset||!Plan.keys->WaitForKey)return EFI_UNSUPPORTED;
  Plan.key_reset=(UINTN)Plan.keys->Reset;Plan.key_read=(UINTN)Plan.keys->ReadKeyStroke;Plan.key_wait=Plan.keys->WaitForKey;
  if(!Path(pci,PciPath,sizeof(PciPath))||!GuAd103ColdEntry())return EFI_NOT_READY;
  EFI_STATUS s=Gops(TRUE);
  if(s==EFI_SUCCESS)s=Alive();
  if(s==EFI_SUCCESS)s=Bar();
  if(s==EFI_SUCCESS)s=ReadConfig(Plan.config);
  if(s==EFI_SUCCESS)s=Gu.pci->Attributes?Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Plan.attributes):EFI_UNSUPPORTED;
  UINT64 decode=(Plan.config[1]&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(Plan.config[1]&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  if(s==EFI_SUCCESS&&(Plan.attributes&(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))!=decode)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Snapshot(&Original);
  if(s==EFI_SUCCESS)s=Consumers();
  if(s==EFI_SUCCESS)s=PointerBaseline();
  if(s==EFI_SUCCESS)s=UsbBaseline();
  GuLog("Z790 preflight: status=%#llx graph=%zu code_profiles=3\n",(unsigned long long)s,(size_t)Original.count);
  if(s==EFI_SUCCESS)GuLog("Z790 endpoints: pci=%#llx gop=%#llx console=%#llx pointer_agent=%#llx\n",
    (unsigned long long)(UINTN)Plan.pci,(unsigned long long)(UINTN)Plan.gop,
    (unsigned long long)(UINTN)Plan.console,(unsigned long long)(UINTN)Plan.pointer_agent);
  if(s==EFI_SUCCESS)Plan.ready=TRUE;return s;
}

static BOOLEAN DecodeMask(const UINT32 c[64],UINT64 *mask) {
  *mask=0;UINT32 missing=Plan.config[1]&~c[1]&3;
  if((Plan.config[1]&6)!=2||(c[1]&4))return FALSE;
  for(UINTN i=0;i<64;i++)if((c[i]|(i==1?missing:0))!=Plan.config[i])return FALSE;
  *mask=(missing&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(missing&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);return TRUE;
}
/* The only place in this port that claims anything exclusively: opens the
 * GPU's PCI_IO protocol EFI_OPEN_PROTOCOL_EXCLUSIVE and enables whatever
 * I/O or memory decode bits the stock driver had left disabled. Every step
 * is checked against the Original graph/config snapshot before and after;
 * any mismatch leaves Gu.owned/Plan.claimed set (so callers can't assume
 * it's safe to pretend this didn't happen) but returns an error. */
static EFI_STATUS ClaimRestore(VOID) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_PCI_IO_PROTOCOL *io=NULL;
  UINT32 c[64];UINT64 mask=0,attrs=0,supported=0;
  EFI_STATUS s=ReadConfig(c);if(s==EFI_SUCCESS&&!DecodeMask(c,&mask))s=EFI_NOT_READY;
  GuLog("Z790 decode restore intent: status=%#llx IO_MEM_mask=%#llx\n",(unsigned long long)s,(unsigned long long)mask);
  EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
  if(s==EFI_SUCCESS)s=ReadConfig(c);
  if(s==EFI_SUCCESS&&!DecodeMask(c,&mask))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(3,FALSE))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=b->OpenProtocol(Plan.pci,&PciGuid,(VOID **)&io,Plan.self,NULL,EFI_OPEN_PROTOCOL_EXCLUSIVE);
  if(s==EFI_SUCCESS){Plan.claimed=TRUE;Gu.owned=TRUE;if(io!=Gu.pci)s=EFI_COMPROMISED_DATA;}
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);
  if(s==EFI_SUCCESS&&attrs!=(Plan.attributes&~mask))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationSupported,0,&supported);
  if(s==EFI_SUCCESS&&(supported&mask)!=mask)s=EFI_UNSUPPORTED;
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(3,TRUE))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS&&mask)s=io->Attributes(io,EfiPciIoAttributeOperationEnable,mask,NULL);
  b->RestoreTPL(old);
  if(s==EFI_SUCCESS)s=ReadConfig(c);
  if(s==EFI_SUCCESS&&!Equal(c,Plan.config,sizeof(c)))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);
  if(s==EFI_SUCCESS&&attrs!=Plan.attributes)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Bar();
  if(s==EFI_SUCCESS&&!GuAd103ColdEntry())s=EFI_NOT_READY;
  GuLog("Z790 exclusive claim/decode result: status=%#llx\n",(unsigned long long)s);return s;
}
/* Detaches the three stock drivers (console-out, then GOP, then the GPU's
 * own PCI_IO/NVIDIA driver) one stage at a time, re-verifying the graph and
 * this exact board's cold-entry state between every stage, then claims the
 * GPU via ClaimRestore above. Any failure at any stage returns immediately
 * -- Plan.mutated is set the moment the first DisconnectController call is
 * made, so callers can tell "nothing touched yet" from "partway through"
 * even on failure. */
EFI_STATUS GuZ790Release(VOID) {
  if(!Plan.ready||Plan.mutated||Gu.started||Gu.owned)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  for(UINTN stage=1;stage<=3;stage++) {
    GuLog("Z790 driver detach intent: stage=%zu\n",(size_t)stage);
    EFI_STATUS s=Alive();UINT32 c[64];
    if(s==EFI_SUCCESS)s=ReadConfig(c);
    if(s==EFI_SUCCESS&&!Equal(c,Plan.config,sizeof(c)))s=EFI_NOT_READY;
    if(s==EFI_SUCCESS&&!GuAd103ColdEntry())s=EFI_NOT_READY;
    if(s==EFI_SUCCESS)s=Gops(FALSE);
    EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
    if(s==EFI_SUCCESS)s=Snapshot(&Current);
    if(s==EFI_SUCCESS&&!Expected(stage-1,FALSE))s=EFI_NOT_READY;
    if(s==EFI_SUCCESS)s=Driver(stage-1);
    b->RestoreTPL(old);
    if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
    /* Record before the call: an error may follow a partial driver stop. */
    Plan.mutated=TRUE;
    s=b->DisconnectController(stage==3?Plan.pci:Plan.gop,
      stage==1?Plan.out_agent:stage==2?Plan.gfx_agent:Plan.nv_agent,NULL);
    GuLog("Z790 driver detach return: stage=%zu status=%#llx\n",(size_t)stage,(unsigned long long)s);
    if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
    Plan.stage=stage;
    s=Snapshot(&Current);
    if(s==EFI_SUCCESS&&!Expected(stage,FALSE))s=EFI_NOT_READY;
    if(s==EFI_SUCCESS)s=Alive();
    if(s==EFI_SUCCESS)s=Gops(FALSE);
    if(s!=EFI_SUCCESS)return s;
  }
  EFI_STATUS s=ClaimRestore();
  if(s==EFI_SUCCESS&&!GuZ790Released())s=EFI_NOT_READY;
  return s;
}

BOOLEAN GuZ790Released(VOID) {
  UINT32 c[64];UINT64 attrs=0;const char *failed=NULL;
  if(!Plan.ready||Plan.stage!=3||!Plan.claimed||!Gu.owned||Gu.fault)failed="state";
  else if(Alive()!=EFI_SUCCESS)failed="console";
  else if(Gops(FALSE)!=EFI_SUCCESS)failed="GOP";
  else if(ReadConfig(c)!=EFI_SUCCESS)failed="config-read";
  else if(!ConfigStable(c))failed="config-drift";
  else if(Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&attrs)!=EFI_SUCCESS||attrs!=Plan.attributes)failed="attributes";
  else if(Snapshot(&Current)!=EFI_SUCCESS)failed="graph-snapshot";
  else if(!Expected(3,TRUE)){failed="graph";GraphDiff(TRUE);}
  if(failed)NOTE("Z790 release check failed: %s attributes=%#llx expected=%#llx graph=%zu original=%zu fault=%u flrs=%u\n",
    failed,(unsigned long long)attrs,(unsigned long long)Plan.attributes,(size_t)Current.count,(size_t)Original.count,Gu.fault,Gu.flrs);
  if(!failed)ReportAdditions();
  return !failed;
}
/* Releases the exclusive PCI_IO claim ClaimRestore took, once the
 * transaction (whatever calls this after GuZ790Release) is done. Only
 * reachable if GuSafeToReturn()/Gu.quiesced/Gu.flrs==1/GuZ790Released() all
 * hold -- i.e. only after a transaction that left the GPU in a safe,
 * verified, post-FLR state. */
EFI_STATUS GuZ790Finish(VOID) {
  if(!GuSafeToReturn()||!Gu.quiesced||Gu.flrs!=1||!GuZ790Released()){NOTE("Z790 finish refused before close\n");return EFI_NOT_READY;}
  EFI_STATUS s=Plan.table->BootServices->CloseProtocol(Plan.pci,&PciGuid,Plan.self,NULL);
  if(s!=EFI_SUCCESS){NOTE("Z790 finish close status=%#llx\n",(unsigned long long)s);return s;}
  Plan.claimed=FALSE;Gu.owned=FALSE;
  s=Snapshot(&Current);if(s==EFI_SUCCESS&&!Expected(3,FALSE)){s=EFI_NOT_READY;GraphDiff(FALSE);}
  if(s!=EFI_SUCCESS)NOTE("Z790 finish after close status=%#llx graph=%zu\n",(unsigned long long)s,(size_t)Current.count);
  return s;
}
BOOLEAN GuZ790Mutated(VOID){return Plan.mutated;}
BOOLEAN GuZ790ReadyForWindows(VOID) {
  if(!Plan.ready||Plan.stage!=3||Plan.claimed||Gu.owned||Gu.fault||!Gu.quiesced||Gu.flrs!=1||!GuSafeToReturn())return FALSE;
  UINT32 c[64];
  if(Alive()!=EFI_SUCCESS||Gops(FALSE)!=EFI_SUCCESS||ReadConfig(c)!=EFI_SUCCESS||!ConfigStable(c)||
     Snapshot(&Current)!=EFI_SUCCESS||!Expected(3,FALSE))return FALSE;
  UINT32 chip=GuRead32(0),cpu=GuRead32(0x110100),hi=GuRead32(0x1fa828);
  UINT32 ss0=GuRead32(0x82381c),ss1=GuRead32(0x823820),readout=GuRead32(0x823818),plm=GuRead32(0x823804);
  /* chip=0x193000a1 is this board's own AD103 id (profile.c's Registers()
   * offset-0 pin), not ad104-msi's AD104 value 0x194000a1. */
  return !Gu.fault&&chip==0x193000a1&&(cpu&0x10)&&cpu!=0xffffffff&&
    (cpu&0xffff0000)!=0xbadf0000&&!(hi>>4)&&ss0==0x88888888&&ss1==8&&
    !(readout&0x07ffffff)&&readout!=0xffffffff&&plm==0xffffffff;
}
