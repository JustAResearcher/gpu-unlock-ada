/* SPDX-License-Identifier: GPL-2.0-only */
/* Real ownership adapter, captured graph and code; all EFI effects simulated. */
#ifndef GU_OWNERSHIP_SOURCE
#define GU_OWNERSHIP_SOURCE "../msi_release.c"
#endif
#include GU_OWNERSHIP_SOURCE
#include "../format.h"
#include "cap4070s_graph.h"
#include "cap4070s_usb_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

enum MODE {OK,WRONG_DRIVER,EXTRA_GPU_OWNER,INPUT_CHANGED,UNRELATED_EDGE_LOST,
 STOP1_FAIL,STOP2_FAIL,STOP3_FAIL,CLAIM_FAIL,ATTR_FAIL,DECODE_EXTRA_BIT,
 CLOSE_FAIL,COLD_CHANGED,
 /* v0.5.1: config/graph drift after release, as seen before FLR in RUN0002. */
 STATUS_DRIFT,FATAL_DRIFT,PARITY_DRIFT,CONTROL_DRIFT,LINK_DRIFT,COMMAND_DRIFT,GRAPH_DRIFT,
 USB_ADDITIONS,USB_TO_GPU,USB_GPU_AGENT,USB_GPU_HANDLE,USB_TO_CONSOLE,USB_DRIVER_AGENT,NEW_EDGE_DUPLICATE,OWN_WRONG_PROTOCOL,PROTECTED_COUNT_CHANGE,
 POINTER_LATE_OK,POINTER_WRONG_AGENT,POINTER_NO_OWNER,POINTER_DUP_OWNER,POINTER_GPU_HANDLE,POINTER_GPU_CONTROLLER,POINTER_WRONG_GUID,POINTER_BAD_COUNT,POINTER_BAD_USB_PATH,POINTER_BAD_USB_HOST,POINTER_CHANGED_INPUT,
 USB_REMOVE_RECONNECT,USB_REMOVE_POINTERS,USB_REMOVE_HOST_DRIVER,USB_REMOVE_HOST_PCI,USB_DYNAMIC_COUNT,USB_OFF_HOST_REMOVAL,USB_REMOVE_INPUT_CHANGED,
 MODE_COUNT};
static unsigned Checks;static enum MODE Mode;static const char *CaseName="fixture-inputs";
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"MSI FAIL case=%s mode=%d line=%d: %s\n",CaseName,Mode,__LINE__,#x);exit(1);}}while(0)
GU_PLATFORM Gu;
static EFI_BOOT_SERVICES Bs;static EFI_SYSTEM_TABLE St;static EFI_PCI_IO_PROTOCOL Io;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Keys;static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Text;
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE GopMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION GopInfo;
static EFI_DRIVER_BINDING_PROTOCOL Bind[3];static EFI_LOADED_IMAGE_PROTOCOL Images[3];
static UINT8 *Code[3],*LoadedCode[3];
static EDGE FixtureEdges[ARRAY_SIZE(CapturedEdges)];
static UINT8 ActualPciPath[sizeof(CapturedPciPath)],ActualGopPath[sizeof(CapturedGopPath)],ActualFile[3][32];static UINT32 Cfg[64];static UINT64 Attrs;
static UINTN Stage,Stops,Enables,Closes,OpenCalls;static BOOLEAN Exclusive,Cold,Drift,SafeReturn;
static UINT32 DecodeDrop,ReadFaultOffset,ReadFaultValue;
static unsigned OwnShape,AttrFault,CorruptAfter,ColdAfter,GraphBefore,BindingUnavailable,ImageUnavailable;
static BOOLEAN GraphInjected,AliasNvImage;
static const UINT8 UsbMousePath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,0x14,3,5,6,0,0,3,127,255,4,0};
static UINT8 BadMousePath[sizeof(UsbMousePath)];
static EDGE NewEdges[16];static UINTN NewCount;
static char Notes[1<<16];static size_t NotesUsed;
static EFI_HANDLE H(UINTN id){return (EFI_HANDLE)(id+1);}
static EFI_HANDLE Agent(UINTN i){return H(CapturedDrivers[i].agent);}
static EFI_STATUS EFIAPI KeyRead(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *p,EFI_INPUT_KEY *key){(void)p;(void)key;return EFI_NOT_READY;}
static EFI_STATUS EFIAPI KeyReset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *p,BOOLEAN verify){(void)p;(void)verify;return EFI_SUCCESS;}
/* The firmware formatter: an unsupported specifier would fault the real transaction. */
VOID GuLog(const char *f,...){
  char line[2048];va_list a;va_start(a,f);size_t n=GuFormatV(line,sizeof(line),f,a);va_end(a);
  CHECK(n!=(size_t)-1&&n<sizeof(line)&&NotesUsed+n<sizeof(Notes));memcpy(Notes+NotesUsed,line,n);NotesUsed+=n;Notes[NotesUsed]=0;
}
VOID GuLogText(const char *s){(void)s;}
BOOLEAN GuBootLogOpen(VOID){return TRUE;}
BOOLEAN GuTargetConfig(const UINT32 *c,UINTN s,UINTN b,UINTN d,UINTN f){return c[0]==0x278310deu&&c[11]==0x413b1458u&&!s&&b==1&&!d&&!f;}
BOOLEAN GuAd104ColdEntry(VOID){return Cold;}
BOOLEAN GuSafeToReturn(VOID){return SafeReturn;}
UINT32 GuRead32(UINT32 off){if(off==ReadFaultOffset)return ReadFaultValue;switch(off){case 0:return 0x194000a1;case 0x110100:return 0x10;case 0x82381c:return 0x88888888;case 0x823820:return 8;case 0x823804:return 0xffffffff;default:return 0;}}
static EFI_STATUS EFIAPI Free(VOID *p){free(p);return EFI_SUCCESS;}
static BOOLEAN Filter(const EDGE *e) {
  if(Drift&&Mode==USB_REMOVE_POINTERS&&(e->handle==H(412)||e->handle==H(415)||
     (e->handle==H(380)&&(e->controller==H(412)||e->controller==H(415)))))return TRUE;
  if(Drift&&Mode==USB_REMOVE_HOST_DRIVER&&e->handle==H(380)&&Guid(&e->protocol,&UsbHcGuid)&&e->attributes==0x10)return TRUE;
  if(Drift&&Mode==USB_REMOVE_HOST_PCI&&e->handle==H(380)&&Guid(&e->protocol,&PciGuid))return TRUE;
  if(Stage>=1&&e->handle==H(407)&&e->agent==Agent(0)&&Guid(&e->protocol,&TextGuid))return TRUE;
  if(Stage>=2&&e->handle==H(407)&&e->agent==Agent(1)&&Guid(&e->protocol,&GopGuid))return TRUE;
  return Stage>=3&&e->handle==H(374)&&e->agent==Agent(2);
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE kind,EFI_GUID *g,VOID *key,UINTN *count,EFI_HANDLE **h) {
  (void)key;*h=NULL;
  if(kind==AllHandles){*count=CapturedHandleCount;*h=malloc(*count*sizeof(**h));CHECK(*h);for(UINTN i=0;i<*count;i++)(*h)[i]=H(i);return EFI_SUCCESS;}
  CHECK(kind==ByProtocol&&Guid(g,&GopGuid));
  if(Stage>=3){*count=0;return EFI_NOT_FOUND;}
  *count=1;*h=malloc(sizeof(**h));CHECK(*h);(*h)[0]=H(407);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *g,VOID **p) {
  *p=NULL;
  if(Guid(g,&PathGuid)){if(h==H(374))*p=(VOID *)ActualPciPath;else if(h==H(407)&&Stage<3)*p=(VOID *)ActualGopPath;}
  if(Guid(g,&PathGuid))for(UINTN i=0;i<ARRAY_SIZE(CapturedUsbPaths);i++)
    if(h==CapturedUsbPaths[i].handle)*p=(VOID *)CapturedUsbPaths[i].path;
  if(Guid(g,&PathGuid)&&(h==H(457)||h==H(456))&&Mode>=POINTER_LATE_OK)
    *p=(VOID *)((Mode==POINTER_BAD_USB_PATH||Mode==POINTER_BAD_USB_HOST||Mode==USB_OFF_HOST_REMOVAL)?BadMousePath:UsbMousePath);
  if(Guid(g,&PathGuid)&&Mode==USB_REMOVE_RECONNECT&&Drift&&!NewCount&&(h==H(456)||h==H(457)))*p=NULL;
  if(Guid(g,&PciGuid)&&h==H(374))*p=&Io;
  if(Guid(g,&GopGuid)&&h==H(407)&&Stage<3)*p=&Gop;
  if(Guid(g,&InputGuid)&&h==H(313))*p=&Keys;
  if(Guid(g,&TextGuid)&&h==H(313))*p=&Text;
  for(UINTN i=0;i<3;i++) {
    if(Guid(g,&BindingGuid)&&h==Agent(i)&&i!=BindingUnavailable)*p=&Bind[i];
    if(Guid(g,&ImageGuid)&&h==H(CapturedDrivers[i].image)&&i!=ImageUnavailable)*p=&Images[i];
  }
  if(Guid(g,&ImageGuid)&&AliasNvImage&&h==H(449))*p=&Images[2];
  return *p?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Protocols(EFI_HANDLE h,EFI_GUID ***out,UINTN *n) {
  EFI_GUID *unique[64];*n=0;
  for(UINTN i=0;i<ARRAY_SIZE(CapturedEdges);i++)if(FixtureEdges[i].handle==h&&!Filter(&FixtureEdges[i])) {
    BOOLEAN seen=FALSE;for(UINTN j=0;j<*n;j++)if(Guid(unique[j],&FixtureEdges[i].protocol))seen=TRUE;
    if(!seen)unique[(*n)++]=(EFI_GUID *)&FixtureEdges[i].protocol;
  }
  if(Exclusive&&h==H(374)){BOOLEAN seen=FALSE;for(UINTN j=0;j<*n;j++)if(Guid(unique[j],&PciGuid))seen=TRUE;if(!seen)unique[(*n)++]=&PciGuid;}
  for(UINTN i=0;i<NewCount;i++)if(NewEdges[i].handle==h) {
    BOOLEAN seen=FALSE;for(UINTN j=0;j<*n;j++)if(Guid(unique[j],&NewEdges[i].protocol))seen=TRUE;
    if(!seen)unique[(*n)++]=&NewEdges[i].protocol;
  }
  if(!*n)unique[(*n)++]=&PathGuid;
  *out=malloc(*n*sizeof(**out));CHECK(*out);memcpy(*out,unique,*n*sizeof(**out));return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Opens(EFI_HANDLE h,EFI_GUID *g,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *n) {
  *out=calloc(128,sizeof(**out));CHECK(*out);*n=0;
  for(UINTN i=0;i<ARRAY_SIZE(CapturedEdges);i++) {
    const EDGE *e=&FixtureEdges[i];if(e->handle!=h||!Guid(g,&e->protocol)||Filter(e))continue;
    if(Mode==UNRELATED_EDGE_LOST&&Stage&&i==0)continue;
    (*out)[(*n)++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){e->agent,e->controller,e->attributes,e->count};
  }
  if(Exclusive&&h==H(374)&&Guid(g,&PciGuid)) {
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY own={H(450),NULL,EFI_OPEN_PROTOCOL_EXCLUSIVE,1};
    if(OwnShape==1)own.AgentHandle=H(449);
    if(OwnShape==2)own.ControllerHandle=H(374);
    if(OwnShape==3)own.OpenCount=2;
    if(OwnShape==4)own.Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;
    (*out)[(*n)++]=own;if(OwnShape==5)(*out)[(*n)++]=own;
  }
  if(Mode==EXTRA_GPU_OWNER&&h==H(374)&&Guid(g,&PciGuid))(*out)[(*n)++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){H(449),H(374),0x10,1};
  if(Mode==GRAPH_DRIFT&&Drift&&h==H(374)&&Guid(g,&PciGuid))(*out)[(*n)++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){H(448),H(374),0x10,1};
  for(UINTN i=0;i<NewCount;i++)if(NewEdges[i].handle==h&&Guid(g,&NewEdges[i].protocol)) {
    EDGE *e=&NewEdges[i];(*out)[(*n)++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){e->agent,e->controller,e->attributes,e->count};
  }
  if(Mode==PROTECTED_COUNT_CHANGE&&Drift&&h==H(374)&&Guid(g,&PciGuid)&&*n)(*out)[0].OpenCount++;
  CHECK(*n<=128);return EFI_SUCCESS;
}
static EFI_TPL EFIAPI Raise(EFI_TPL t){
  CHECK(t==TPL_NOTIFY);
  /* Drift enters the next pre-Stop snapshot, after the prior Stop's postcheck. */
  if(GraphBefore==Stage){
    NewCount=1;NewEdges[0]=(EDGE){H(374),H(449),H(374),PciGuid,0x10,1};
    GraphInjected=TRUE;GraphBefore=~0u;
  }
  return TPL_APPLICATION;
}
static VOID EFIAPI Restore(EFI_TPL t){CHECK(t==TPL_APPLICATION);}
static EFI_STATUS EFIAPI Location(EFI_PCI_IO_PROTOCOL *p,UINTN *s,UINTN *b,UINTN *d,UINTN *f){CHECK(p==&Io);*s=*d=*f=0;*b=1;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI ConfigRead(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN count,VOID *out){CHECK(p==&Io&&w==EfiPciIoWidthUint32&&!off&&count==64);memcpy(out,Cfg,sizeof(Cfg));return EFI_SUCCESS;}
static EFI_STATUS EFIAPI GetBar(EFI_PCI_IO_PROTOCOL *p,UINT8 index,UINT64 *support,VOID **out){
  CHECK(p==&Io&&!index);*support=0;EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=calloc(1,sizeof(*d));CHECK(d);d->Desc=0x8a;d->Len=43;d->AddrRangeMin=GU_BAR0_BASE;d->AddrRangeMax=0x80ffffff;d->AddrLen=0x1000000;*out=d;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Attributes(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 mask,UINT64 *out){
  CHECK(p==&Io);
  if(op==EfiPciIoAttributeOperationGet){CHECK(out&&!mask);if(AttrFault==1&&Exclusive)return EFI_DEVICE_ERROR;*out=Attrs;return EFI_SUCCESS;}
  if(op==EfiPciIoAttributeOperationSupported){CHECK(out&&!mask);if(AttrFault==2)return EFI_DEVICE_ERROR;*out=AttrFault==3?0:EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;return EFI_SUCCESS;}
  UINT64 expected=(DecodeDrop&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(DecodeDrop&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  CHECK(op==EfiPciIoAttributeOperationEnable&&mask==expected&&!(mask&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER)&&!out&&Exclusive);
  Enables++;if(Mode==ATTR_FAIL)return EFI_DEVICE_ERROR;
  if(AttrFault!=4){Attrs|=mask;Cfg[1]|=DecodeDrop;}
  if(AttrFault==5){Attrs|=EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;Cfg[1]|=4;}
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_HANDLE h,EFI_GUID *g,VOID **out,EFI_HANDLE a,EFI_HANDLE controller,UINT32 attrs){
  CHECK(h==H(374)&&Guid(g,&PciGuid)&&a==H(450)&&!controller&&attrs==EFI_OPEN_PROTOCOL_EXCLUSIVE&&Stage==3&&!Exclusive);
  OpenCalls++;if(Mode==CLAIM_FAIL)return EFI_ACCESS_DENIED;Exclusive=TRUE;*out=OwnShape==6?(VOID *)&Bs:(VOID *)&Io;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_HANDLE h,EFI_GUID *g,EFI_HANDLE a,EFI_HANDLE controller){
  CHECK(h==H(374)&&Guid(g,&PciGuid)&&a==H(450)&&!controller&&Exclusive);Closes++;
  if(Mode==CLOSE_FAIL)return EFI_DEVICE_ERROR;Exclusive=FALSE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Disconnect(EFI_HANDLE controller,EFI_HANDLE driver,EFI_HANDLE child){
  CHECK(!child&&Stops<3&&GuMsiMutated());CHECK(controller==(Stops==2?H(374):H(407))&&driver==Agent(Stops));Stops++;
  Stage=Stops;
  if(Stage==3){Cfg[1]&=~DecodeDrop;Attrs&=~((DecodeDrop&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(DecodeDrop&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0));if(Mode==DECODE_EXTRA_BIT)Cfg[2]^=1;}
  if(CorruptAfter==Stage&&Stage<3)Code[Stage][CapturedDrivers[Stage].rva]^=1;
  if(ColdAfter==Stage)Cold=FALSE;
  if(Mode==INPUT_CHANGED&&Stage==1)St.ConIn=NULL;
  if(Mode==COLD_CHANGED&&Stage==3)Cold=FALSE;
  if((Mode==STOP1_FAIL&&Stage==1)||(Mode==STOP2_FAIL&&Stage==2)||(Mode==STOP3_FAIL&&Stage==3))return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
static void Setup(void) {
  memset(&Gu,0,sizeof(Gu));memset(&Plan,0,sizeof(Plan));memset(&Bs,0,sizeof(Bs));memset(&St,0,sizeof(St));memset(&Io,0,sizeof(Io));
  memset(&Keys,0,sizeof(Keys));memcpy(Cfg,CapturedConfig4070S,sizeof(Cfg));Stage=Stops=Enables=Closes=OpenCalls=0;Exclusive=Drift=FALSE;Cold=SafeReturn=TRUE;
  memcpy(FixtureEdges,CapturedEdges,sizeof(FixtureEdges));memcpy(ActualPciPath,CapturedPciPath,sizeof(ActualPciPath));memcpy(ActualGopPath,CapturedGopPath,sizeof(ActualGopPath));
  DecodeDrop=3;ReadFaultOffset=0xffffffff;ReadFaultValue=0;OwnShape=AttrFault=CorruptAfter=ColdAfter=0;BindingUnavailable=ImageUnavailable=3;
  GraphBefore=~0u;GraphInjected=AliasNvImage=FALSE;
  NotesUsed=0;Notes[0]=0;NewCount=0;memcpy(BadMousePath,UsbMousePath,sizeof(UsbMousePath));
  Bs.LocateHandleBuffer=Locate;Bs.HandleProtocol=Handle;Bs.ProtocolsPerHandle=Protocols;Bs.OpenProtocolInformation=Opens;Bs.FreePool=Free;
  Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.OpenProtocol=Open;Bs.CloseProtocol=Close;Bs.DisconnectController=Disconnect;
  St.BootServices=&Bs;St.ConsoleInHandle=St.ConsoleOutHandle=St.StandardErrorHandle=H(313);St.ConIn=&Keys;St.ConOut=St.StdErr=&Text;
  Keys.ReadKeyStroke=KeyRead;Keys.Reset=KeyReset;Keys.WaitForKey=(EFI_EVENT)1;
  memset(&GopMode,0,sizeof(GopMode));memset(&GopInfo,0,sizeof(GopInfo));
  Gop.Mode=&GopMode;GopMode.Info=&GopInfo;GopMode.SizeOfInfo=sizeof(GopInfo);GopMode.FrameBufferBase=0x4000000000;GopMode.FrameBufferSize=0x21c0000;
  GopInfo.HorizontalResolution=3840;GopInfo.VerticalResolution=2160;
  Io.Pci.Read=ConfigRead;Io.GetLocation=Location;Io.GetBarAttributes=GetBar;Io.Attributes=Attributes;
  Attrs=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
  Gu.bs=&Bs;Gu.pci=&Io;Gu.image=H(450);Gu.handle=H(374);Gu.profile_verified=Gu.prom_verified=TRUE;
  for(UINTN i=0;i<3;i++) {
    memset(&Bind[i],0,sizeof(Bind[i]));memset(&Images[i],0,sizeof(Images[i]));
    memset(Code[i],0,CapturedDrivers[i].size);memcpy(Code[i]+CapturedDrivers[i].rva,LoadedCode[i],CapturedDrivers[i].bytes);
    memcpy(ActualFile[i],CapturedDrivers[i].file,sizeof(ActualFile[i]));
    Bind[i].DriverBindingHandle=Agent(i);Bind[i].ImageHandle=H(CapturedDrivers[i].image);Bind[i].Version=CapturedDrivers[i].version;
    Bind[i].Stop=(EFI_DRIVER_BINDING_STOP)(UINTN)(Code[i]+CapturedDrivers[i].stop);
    Images[i].ImageBase=Code[i];Images[i].ImageSize=CapturedDrivers[i].size;Images[i].ImageCodeType=EfiBootServicesCode;
    Images[i].DeviceHandle=H(CapturedDrivers[i].source);Images[i].FilePath=(VOID *)ActualFile[i];
  }
  if(Mode==WRONG_DRIVER)Bind[1].Version++;
  if(Mode==USB_REMOVE_RECONNECT||Mode==USB_DYNAMIC_COUNT||Mode==USB_REMOVE_INPUT_CHANGED) {
    EFI_GUID usbio={0x2b2f68d6,0x0cd2,0x44cf,{0x8e,0x8b,0xbb,0xa2,0x0b,0x1b,0x5b,0x75}};
    EFI_GUID keyex={0xdd9e7534,0x7762,0x4698,{0x8c,0x14,0xf5,0x85,0x17,0xa6,0x25,0xaa}};
    NewCount=9;
    NewEdges[0]=(EDGE){H(380),H(83),H(457),UsbHcGuid,8,1};
    NewEdges[1]=(EDGE){H(457),H(84),H(457),usbio,0x10,1};
    NewEdges[2]=(EDGE){H(457),H(312),H(457),PointerGuid,0x10,1};
    NewEdges[3]=(EDGE){H(457),H(312),H(313),PointerGuid,8,1};
    NewEdges[4]=(EDGE){H(380),H(83),H(456),UsbHcGuid,8,1};
    NewEdges[5]=(EDGE){H(456),H(84),H(456),usbio,0x10,1};
    NewEdges[6]=(EDGE){H(456),H(311),H(456),InputGuid,0x10,1};
    NewEdges[7]=(EDGE){H(456),H(311),H(456),keyex,0x10,1};
    NewEdges[8]=(EDGE){H(456),H(311),H(313),InputGuid,8,1};
  }
  if(Mode==USB_OFF_HOST_REMOVAL) {
    BadMousePath[17]=1;NewCount=1;NewEdges[0]=(EDGE){H(380),H(83),H(457),UsbHcGuid,8,1};
  }
}
static void LegacyCases(void) {
  CaseName="retained-ownership";
  for(Mode=OK;Mode<MODE_COUNT;Mode++) {
    Setup();EFI_STATUS s=GuMsiPreflight(&St,H(450),H(374));
    if(Mode==WRONG_DRIVER||Mode==EXTRA_GPU_OWNER){CHECK(s!=EFI_SUCCESS&&!Stops&&!GuMsiMutated()&&!GuMsiReadyForWindows());printf("MSI admission case %d PASS\n",Mode);continue;}
    CHECK(s==EFI_SUCCESS);CHECK(!GuMsiReadyForWindows());s=GuMsiRelease();
    if(Mode>=USB_REMOVE_RECONNECT) {
      CHECK(s==EFI_SUCCESS&&GuMsiReleased()&&Plan.usb_host==H(380)&&Plan.usb_agent==H(83));
      Drift=TRUE;
      if(Mode==USB_REMOVE_RECONNECT||Mode==USB_REMOVE_INPUT_CHANGED) {
        CHECK(Original.count==ARRAY_SIZE(CapturedEdges)+9);NewCount=0;CHECK(Plan.key_agent==H(311));
      }
      if(Mode==USB_DYNAMIC_COUNT)NewEdges[0].count=2;
      if(Mode==USB_OFF_HOST_REMOVAL)NewCount=0;
      if(Mode==USB_REMOVE_INPUT_CHANGED)St.ConIn=NULL;
      BOOLEAN okay=Mode==USB_REMOVE_RECONNECT||Mode==USB_REMOVE_POINTERS;
      if(okay) {
        CHECK(GuMsiReleased());CHECK(Plan.reported_removals>0);
        if(Mode==USB_REMOVE_RECONNECT) {
          /* Exact capture: 121 + 9 original -> 117 current after 5 GPU removals and 1 claim. */
          CHECK(Current.count==ARRAY_SIZE(CapturedEdges)-4);NewCount=9;CHECK(GuMsiReleased());NewCount=0;CHECK(GuMsiReleased());
        }
        Gu.quiesced=TRUE;Gu.flrs=1;CHECK(GuMsiFinish()==EFI_SUCCESS&&!Exclusive);CHECK(GuMsiReadyForWindows());
      } else {
        CHECK(!GuMsiReleased());Gu.quiesced=TRUE;Gu.flrs=1;
        CHECK(GuMsiFinish()!=EFI_SUCCESS&&Exclusive);CHECK(!GuMsiReadyForWindows());
      }
      printf("MSI USB lifecycle case %d PASS\n",Mode);continue;
    }
    if(Mode>=POINTER_LATE_OK) {
      CHECK(s==EFI_SUCCESS&&GuMsiReleased()&&Plan.pointer_agent==H(312));
      NewCount=2;
      NewEdges[0]=(EDGE){H(457),H(312),H(457),PointerGuid,EFI_OPEN_PROTOCOL_BY_DRIVER,1};
      NewEdges[1]=(EDGE){H(457),H(312),H(313),PointerGuid,EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER,1};
      if(Mode==POINTER_WRONG_AGENT){NewEdges[0].agent=NewEdges[1].agent=H(439);}
      if(Mode==POINTER_NO_OWNER){NewCount=1;NewEdges[0]=NewEdges[1];}
      if(Mode==POINTER_DUP_OWNER){NewCount=3;NewEdges[2]=NewEdges[0];}
      if(Mode==POINTER_GPU_HANDLE)NewEdges[1].handle=H(374);
      if(Mode==POINTER_GPU_CONTROLLER)NewEdges[1].controller=H(374);
      if(Mode==POINTER_WRONG_GUID)NewEdges[1].protocol.Data2++;
      if(Mode==POINTER_BAD_COUNT)NewEdges[1].count++;
      if(Mode==POINTER_BAD_USB_PATH)BadMousePath[19]=4;
      if(Mode==POINTER_BAD_USB_HOST)BadMousePath[17]=1;
      if(Mode==POINTER_CHANGED_INPUT)St.ConIn=NULL;
      if(Mode==POINTER_LATE_OK) {
        CHECK(GuMsiReleased());CHECK(Plan.reported_additions==2);
        Gu.quiesced=TRUE;Gu.flrs=1;CHECK(GuMsiFinish()==EFI_SUCCESS&&!Exclusive);
        CHECK(GuMsiReadyForWindows());
      } else {
        CHECK(!GuMsiReleased());Gu.quiesced=TRUE;Gu.flrs=1;
        CHECK(GuMsiFinish()!=EFI_SUCCESS&&Exclusive);CHECK(!GuMsiReadyForWindows());
      }
      printf("MSI late pointer case %d PASS\n",Mode);continue;
    }
    if(Mode>=USB_ADDITIONS) {
      CHECK(s==EFI_SUCCESS&&GuMsiReleased());
      EFI_GUID usb={0x3e745226,0x9818,0x45b6,{0xa2,0xac,0xd7,0xcd,0x0e,0x8b,0xa2,0xbc}};
      Drift=TRUE;NewCount=1;NewEdges[0]=(EDGE){H(457),H(440),H(441),usb,8,1};
      if(Mode==USB_TO_GPU)NewEdges[0].controller=H(374);
      if(Mode==USB_GPU_AGENT)NewEdges[0].agent=H(374);
      if(Mode==USB_GPU_HANDLE)NewEdges[0].handle=H(374);
      if(Mode==USB_TO_CONSOLE)NewEdges[0].controller=H(313);
      if(Mode==USB_DRIVER_AGENT)NewEdges[0].agent=Agent(2);
      if(Mode==NEW_EDGE_DUPLICATE){NewCount=2;NewEdges[1]=NewEdges[0];}
      if(Mode==OWN_WRONG_PROTOCOL)NewEdges[0]=(EDGE){H(374),H(450),NULL,usb,EFI_OPEN_PROTOCOL_EXCLUSIVE,1};
      if(Mode==PROTECTED_COUNT_CHANGE)NewCount=0;
      if(Mode==USB_ADDITIONS) {
        /* Unrelated USB additions, alongside the exclusive GPU claim. */
        NewCount=5;for(UINTN k=1;k<NewCount;k++){NewEdges[k]=NewEdges[0];NewEdges[k].controller=H(441+k);}
        CHECK(GuMsiReleased());Gu.quiesced=TRUE;Gu.flrs=1;
        CHECK(GuMsiFinish()==EFI_SUCCESS&&!Exclusive);CHECK(GuMsiReadyForWindows());
      } else {
        CHECK(!GuMsiReleased());Gu.quiesced=TRUE;Gu.flrs=1;
        CHECK(GuMsiFinish()!=EFI_SUCCESS&&Exclusive);CHECK(!GuMsiReadyForWindows());
      }
      printf("MSI enumeration case %d PASS\n",Mode);continue;
    }
    if(Mode>=STATUS_DRIFT) {
      CHECK(s==EFI_SUCCESS&&Stops==3&&Exclusive&&GuMsiReleased());
      UINT32 *devsta=&Cfg[0x80/4],*linksta=&Cfg[0x88/4];
      CHECK(Cfg[0x78/4]==0x0012b410&&*devsta==0x00092930&&*linksta==0x11040140);
      /* Pre-FLR drift, as the firmware run produced before the aperture check. */
      Drift=TRUE;NotesUsed=0;Notes[0]=0;
      if(Mode==STATUS_DRIFT){Cfg[1]|=0x00080000;*devsta=(*devsta|0x00020000)&~0x00010000u;*linksta|=0x40000000;}
      if(Mode==FATAL_DRIFT)*devsta|=0x00040000;
      if(Mode==PARITY_DRIFT)Cfg[1]|=0x80000000;
      if(Mode==CONTROL_DRIFT)*devsta^=0x00000010;
      if(Mode==LINK_DRIFT)*linksta=(*linksta&~0x000f0000u)|0x00010000;
      if(Mode==COMMAND_DRIFT)Cfg[1]|=4;
      if(Mode==STATUS_DRIFT) {
        CHECK(!GuMsiReleased());
        memcpy(Cfg,Plan.config,sizeof(Cfg));*devsta&=~0x00090000u;
        CHECK(!GuMsiReleased()); /* Even preexisting bits may clear only after FLR. */
        Gu.quiesced=TRUE;Gu.flrs=1;NotesUsed=0;Notes[0]=0;
        CHECK(GuMsiReleased());
        CHECK(GuMsiFinish()==EFI_SUCCESS&&!Exclusive&&Closes==1);CHECK(GuMsiReadyForWindows());
      } else {
        CHECK(!GuMsiReleased());
        Gu.quiesced=TRUE;Gu.flrs=1;CHECK(GuMsiFinish()!=EFI_SUCCESS&&Exclusive&&!Closes);
        CHECK(!GuMsiReadyForWindows());
      }
      printf("MSI drift case %d PASS\n",Mode);
      continue;
    }
    if(Mode==OK||Mode==CLOSE_FAIL) {
      CHECK(s==EFI_SUCCESS&&Stops==3&&Enables==1&&Exclusive&&Gu.owned&&GuMsiReleased());
      Gu.quiesced=TRUE;Gu.flrs=1;s=GuMsiFinish();
      if(Mode==OK){CHECK(s==EFI_SUCCESS&&!Exclusive&&!Gu.owned&&Closes==1);CHECK(GuMsiReadyForWindows());Gu.fault=TRUE;CHECK(!GuMsiReadyForWindows());Gu.fault=FALSE;Stage=2;CHECK(!GuMsiReadyForWindows());Stage=3;}
      else CHECK(s!=EFI_SUCCESS&&Exclusive&&Closes==1&&!GuMsiReadyForWindows());
    } else {
      CHECK(s!=EFI_SUCCESS&&GuMsiMutated()&&!GuMsiReadyForWindows());CHECK(Stops<=3);
      if(Mode==INPUT_CHANGED||Mode==UNRELATED_EDGE_LOST)CHECK(Stops==1&&!OpenCalls&&!Enables&&!Exclusive&&!Gu.owned);
      if(Mode>=STOP1_FAIL&&Mode<=STOP3_FAIL)CHECK(Stops==(UINTN)(Mode-STOP1_FAIL+1)&&!OpenCalls);
      if(Mode==DECODE_EXTRA_BIT||Mode==CLAIM_FAIL)CHECK(!Enables);
      /* The app must halt on this failed release plus mutated=true. A cold
       * check failure does not itself invalidate the narrower graph predicate. */
      if(Mode!=COLD_CHANGED){CHECK(!GuMsiReleased());Gu.quiesced=TRUE;Gu.flrs=1;CHECK(GuMsiFinish()!=EFI_SUCCESS&&!Closes);}
    }
    printf("MSI release case %d PASS stops=%zu decode_writes=%zu\n",Mode,(size_t)Stops,(size_t)Enables);
  }
  CHECK(MODE_COUNT==47);printf("MSI: %u checks across %u retained cases; captured graph/code, all callbacks simulated.\n",Checks,MODE_COUNT);
}

/* The fixture is independent of production pins. This also runs unchanged
 * against the original source, so a stale GOP ADR cannot be hidden by mocks. */
static void CapturedAdmission(void) {
  CaseName="captured-admission";Mode=OK;Setup();
  EFI_STATUS s=GuMsiPreflight(&St,H(450),H(374));
  printf("CAPTURE preflight status=%#llx handles=%u graph=%zu mutated=%u stops=%zu\n",
    (unsigned long long)s,CapturedHandleCount,(size_t)Original.count,GuMsiMutated(),(size_t)Stops);fflush(stdout);
  CHECK(s==EFI_SUCCESS&&!GuMsiMutated()&&!Stops&&!OpenCalls);
  CHECK(Original.count==121&&Plan.pci==H(374)&&Plan.gop==H(407)&&Plan.console==H(313));
  CHECK(Plan.out_agent==H(310)&&Plan.gfx_agent==H(315)&&Plan.nv_agent==H(406));
  CHECK(Plan.pointer_agent==H(312)&&Plan.key_agent==H(311)&&Plan.usb_host==H(380)&&Plan.usb_agent==H(83));
  CHECK(!memcmp(Cfg,CapturedConfig4070S,sizeof(Cfg))&&!GuMsiReadyForWindows());
}

/* White-box isolation supplements public preflight: report all three pin
 * results even when the old source refuses the earlier GOP-path check. */
static void DriverAdmission(void) {
  CaseName="captured-driver-pins";Mode=OK;Setup();
  Plan.table=&St;Plan.pci=H(374);Plan.out_agent=Agent(0);Plan.gfx_agent=Agent(1);Plan.nv_agent=Agent(2);
  EFI_STATUS statuses[3];
  for(UINTN i=0;i<3;i++) {
    statuses[i]=Driver(i);
    printf("CAPTURE driver=%zu agent=%zu status=%#llx size=%zu code_bytes=%zu stop_rva=%#zx\n",
      (size_t)i,(size_t)CapturedDrivers[i].agent,(unsigned long long)statuses[i],
      (size_t)CapturedDrivers[i].size,(size_t)CapturedDrivers[i].bytes,(size_t)CapturedDrivers[i].stop);
  }
  fflush(stdout);for(UINTN i=0;i<3;i++)CHECK(statuses[i]==EFI_SUCCESS);
  CHECK(!Stops&&!OpenCalls&&!GuMsiMutated());
}

static void RefusedBeforeMutation(void) {
  EFI_STATUS s=GuMsiPreflight(&St,H(450),H(374));
  CHECK(s!=EFI_SUCCESS&&!Stops&&!OpenCalls&&!Enables&&!Closes&&!GuMsiMutated());
  CHECK(GuMsiRelease()!=EFI_SUCCESS&&!GuMsiReleased()&&!GuMsiReadyForWindows());
}

static void PinRejections(void) {
  CapturedAdmission();
  static const char *names[]={"version","null-stop","stop-rva-minus-one","stop-rva-plus-one",
    "image-size-minus-one","image-size-plus-one","old-image-size","fake-binding-handle",
    "missing-binding","null-image-handle","null-image-base","wrong-code-type","null-file-path",
    "changed-file-path","changed-code-start","changed-code-end","changed-stop-code",
    "shifted-code-rva","missing-image-protocol"};
  static const UINTN old_size[]={24576,14816,162016},old_stop[]={0x126c,0xa38};
  unsigned cases=0;char label[96];
  for(UINTN i=0;i<3;i++)for(UINTN k=0;k<ARRAY_SIZE(names);k++) {
    snprintf(label,sizeof(label),"driver-%zu-%s",(size_t)i,names[k]);CaseName=label;Mode=OK;Setup();
    switch(k) {
      case 0:Bind[i].Version++;break;
      case 1:Bind[i].Stop=NULL;break;
      case 2:Bind[i].Stop=(EFI_DRIVER_BINDING_STOP)(UINTN)(Code[i]+CapturedDrivers[i].stop-1);break;
      case 3:Bind[i].Stop=(EFI_DRIVER_BINDING_STOP)(UINTN)(Code[i]+CapturedDrivers[i].stop+1);break;
      case 4:Images[i].ImageSize--;break;
      case 5:Images[i].ImageSize++;break;
      case 6:Images[i].ImageSize=old_size[i];break;
      case 7:Bind[i].DriverBindingHandle=H(449);break;
      case 8:BindingUnavailable=(unsigned)i;break;
      case 9:Bind[i].ImageHandle=NULL;break;
      case 10:Images[i].ImageBase=NULL;break;
      case 11:Images[i].ImageCodeType=EfiBootServicesData;break;
      case 12:Images[i].FilePath=NULL;break;
      case 13:ActualFile[i][4]^=1;break;
      case 14:Code[i][CapturedDrivers[i].rva]^=1;break;
      case 15:Code[i][CapturedDrivers[i].rva+CapturedDrivers[i].bytes-1]^=1;break;
      case 16:Code[i][CapturedDrivers[i].stop]^=1;break;
      case 17:memmove(Code[i]+CapturedDrivers[i].rva+1,Code[i]+CapturedDrivers[i].rva,CapturedDrivers[i].bytes);break;
      case 18:ImageUnavailable=(unsigned)i;break;
      default:CHECK(FALSE);
    }
    RefusedBeforeMutation();cases++;
  }
  for(UINTN i=0;i<2;i++) {
    CaseName="old-text-stop-rva";Setup();Bind[i].Stop=(EFI_DRIVER_BINDING_STOP)(UINTN)(Code[i]+old_stop[i]);
    RefusedBeforeMutation();cases++;
  }
  CaseName="old-nvidia-version";Setup();Bind[2].Version=0x7000b;RefusedBeforeMutation();cases++;
  CaseName="nvidia-source-handle";Setup();Images[2].DeviceHandle=H(5);RefusedBeforeMutation();cases++;
  CaseName="nvidia-source-null";Setup();Images[2].DeviceHandle=NULL;RefusedBeforeMutation();cases++;
  CaseName="nvidia-image-is-not-binding";Setup();Bind[2].ImageHandle=Bind[0].ImageHandle;RefusedBeforeMutation();cases++;
  CaseName="nvidia-image-alias-is-not-binding";Setup();AliasNvImage=TRUE;Bind[2].ImageHandle=H(449);
  VOID *alias=NULL;CHECK(Handle(Bind[2].ImageHandle,&ImageGuid,&alias)==EFI_SUCCESS&&alias==&Images[2]);
  RefusedBeforeMutation();cases++;
  CaseName="old-gop-adr";Setup();ActualGopPath[28]=0x40;RefusedBeforeMutation();cases++;
  CaseName="changed-pci-path";Setup();ActualPciPath[17]^=1;RefusedBeforeMutation();cases++;
  CaseName="gop-multi-instance";Setup();ActualGopPath[sizeof(ActualGopPath)-3]=1;RefusedBeforeMutation();cases++;
  /* Each of the five protected capture edges must retain GUID, attribute and count. */
  for(UINTN i=0;i<ARRAY_SIZE(CapturedEdges);i++)if(CapturedEdges[i].handle==H(374)||CapturedEdges[i].handle==H(407)) {
    for(unsigned k=0;k<3;k++) {
      CaseName="protected-capture-edge";Setup();
      if(k==0)FixtureEdges[i].count++;
      if(k==1)FixtureEdges[i].attributes^=0x18;
      if(k==2)FixtureEdges[i].protocol.Data2^=1;
      RefusedBeforeMutation();cases++;
    }
  }
  printf("MSI pin rejections: %u cases PASS; metadata-only relocation flag verified by capture loader, never inferred from hashes.\n",cases);
  CaseName="pin-rejections-complete";
}

static void ReleaseAdmission(void) {
  Mode=OK;Setup();CHECK(GuMsiPreflight(&St,H(450),H(374))==EFI_SUCCESS);
}
static void ContainedFailure(UINTN expected_stops) {
  CHECK(GuMsiRelease()!=EFI_SUCCESS);CHECK(Stops==expected_stops);
  CHECK(GuMsiMutated()==(expected_stops!=0)&&!GuMsiReleased()&&!GuMsiReadyForWindows());
  Gu.quiesced=TRUE;Gu.flrs=1;CHECK(GuMsiFinish()!=EFI_SUCCESS&&!Closes);
}
static void GuardBoundaries(void) {
  CapturedAdmission();unsigned cases=0;
  for(UINTN before=0;before<3;before++) {
    CaseName="fresh-driver-code-before-stop";ReleaseAdmission();
    if(before)CorruptAfter=(unsigned)before;else Code[0][CapturedDrivers[0].rva]^=1;
    ContainedFailure(before);CHECK(!OpenCalls&&!Enables);cases++;
    CaseName="fresh-cold-state-before-stop";ReleaseAdmission();
    if(before)ColdAfter=(unsigned)before;else Cold=FALSE;
    ContainedFailure(before);CHECK(!OpenCalls&&!Enables);cases++;
    CaseName="fresh-protected-graph-before-stop";ReleaseAdmission();GraphBefore=(unsigned)before;
    ContainedFailure(before);CHECK(GraphInjected&&!OpenCalls&&!Enables&&!Exclusive&&!Gu.owned);cases++;
  }
  for(unsigned shape=1;shape<=6;shape++) {
    CaseName="exclusive-claim-shape";ReleaseAdmission();OwnShape=shape;
    ContainedFailure(3);CHECK(OpenCalls==1&&Exclusive&&Gu.owned&&!Enables);cases++;
  }
  for(unsigned fault=1;fault<=5;fault++) {
    CaseName="attribute-restoration-fault";ReleaseAdmission();AttrFault=fault;
    ContainedFailure(3);CHECK(OpenCalls==1&&Exclusive&&Gu.owned);cases++;
  }
  for(UINT32 drop=0;drop<4;drop++) {
    CaseName="restore-only-missing-decode";ReleaseAdmission();DecodeDrop=drop;
    CHECK(GuMsiRelease()==EFI_SUCCESS&&Stops==3&&OpenCalls==1&&Enables==(drop!=0));
    CHECK(!(Cfg[1]&4)&&!(Attrs&EFI_PCI_IO_ATTRIBUTE_BUS_MASTER));
    CHECK(!memcmp(Cfg,CapturedConfig4070S,sizeof(Cfg))&&GuMsiReleased());
    CHECK(!GuMsiReadyForWindows()&&GuMsiFinish()!=EFI_SUCCESS&&!Closes);
    Gu.quiesced=TRUE;Gu.flrs=1;CHECK(GuMsiFinish()==EFI_SUCCESS&&Closes==1&&!Exclusive);
    CHECK(GuMsiReadyForWindows());cases++;
  }
  static const UINTN protected_ids[]={374,407,450,310,315,406,313};
  for(UINTN i=0;i<ARRAY_SIZE(protected_ids);i++)for(unsigned endpoint=0;endpoint<3;endpoint++) {
    CaseName="new-edge-protected-endpoint";ReleaseAdmission();CHECK(GuMsiRelease()==EFI_SUCCESS&&GuMsiReleased());
    NewCount=1;NewEdges[0]=(EDGE){H(457),H(448),H(449),UsbHcGuid,8,1};
    if(endpoint==0)NewEdges[0].handle=H(protected_ids[i]);
    if(endpoint==1)NewEdges[0].agent=H(protected_ids[i]);
    if(endpoint==2)NewEdges[0].controller=H(protected_ids[i]);
    CHECK(!GuMsiReleased()&&!GuMsiReadyForWindows());Gu.quiesced=TRUE;Gu.flrs=1;
    CHECK(GuMsiFinish()!=EFI_SUCCESS&&!Closes&&Exclusive);cases++;
  }
  printf("MSI release boundaries: %u cases PASS; callbacks simulated, Stop code never invoked.\n",cases);
}

static void Finished(void) {
  ReleaseAdmission();CHECK(GuMsiRelease()==EFI_SUCCESS&&GuMsiReleased());
  Gu.quiesced=TRUE;Gu.flrs=1;CHECK(GuMsiFinish()==EFI_SUCCESS&&!Gu.owned&&!Exclusive&&Closes==1);
  CHECK(GuMsiReadyForWindows());
}
static void HandoffBoundaries(void) {
  CapturedAdmission();unsigned cases=0;
  for(unsigned k=0;k<14;k++) {
    CaseName="terminal-handoff-state";Finished();
    switch(k) {
      case 0:Gu.quiesced=FALSE;break;
      case 1:Gu.flrs=0;break;
      case 2:Gu.flrs=2;break;
      case 3:SafeReturn=FALSE;break;
      case 4:Gu.fault=TRUE;break;
      case 5:Gu.owned=TRUE;break;
      case 6:NewCount=1;NewEdges[0]=(EDGE){H(374),H(449),H(374),PciGuid,0x10,1};break;
      case 7:St.ConIn=NULL;break;
      case 8:St.ConOut=NULL;break;
      case 9:Keys.Reset=NULL;break;
      case 10:Keys.ReadKeyStroke=NULL;break;
      case 11:Keys.WaitForKey=NULL;break;
      case 12:St.ConsoleOutHandle=H(449);break;
      case 13:Stage=2;break;
      default:CHECK(FALSE);
    }
    CHECK(!GuMsiReadyForWindows()&&Closes==1&&Stops==3);cases++;
  }
  static const struct {UINT32 offset,value;} bad[]={
    {0,0},{0x110100,0},{0x110100,0xffffffff},{0x110100,0xbadf0010},{0x1fa828,0x10},
    {0x82381c,0},{0x823820,0},{0x823818,1},{0x823818,0xffffffff},{0x823804,0}};
  for(UINTN i=0;i<ARRAY_SIZE(bad);i++) {
    CaseName="terminal-fresh-register-readback";Finished();ReadFaultOffset=bad[i].offset;ReadFaultValue=bad[i].value;
    CHECK(!GuMsiReadyForWindows()&&Closes==1&&Stops==3);cases++;
  }
  printf("MSI terminal handoff: %u cases PASS; predicate only, no Windows/UEFI execution.\n",cases);
}

int main(int argc,char **argv) {
  CHECK(argc==2||argc==3);const char *group=argc==3?argv[2]:"all";
  CHECK(!strcmp(group,"all")||!strcmp(group,"fixtures")||!strcmp(group,"capture")||!strcmp(group,"driver-pins")||
    !strcmp(group,"pin-rejections")||!strcmp(group,"legacy")||!strcmp(group,"guards")||!strcmp(group,"handoff"));
  for(UINTN i=0;i<3;i++) {
    char name[4096];int n=snprintf(name,sizeof(name),"%s/driver-%zu-code.bin",argv[1],(size_t)CapturedDrivers[i].agent);
    CHECK(n>0&&(size_t)n<sizeof(name));FILE *f=fopen(name,"rb");CHECK(f);
    UINTN bytes=CapturedDrivers[i].bytes;LoadedCode[i]=malloc(bytes);Code[i]=calloc(1,CapturedDrivers[i].size);
    CHECK(LoadedCode[i]&&Code[i]);CHECK(fread(LoadedCode[i],1,bytes,f)==bytes&&fgetc(f)==EOF);CHECK(!fclose(f));
    CHECK(GuHashMatches(LoadedCode[i],bytes,CapturedDrivers[i].sha256));
    CHECK(CapturedDrivers[i].rva+bytes<=CapturedDrivers[i].size&&CapturedDrivers[i].stop>=CapturedDrivers[i].rva&&
      CapturedDrivers[i].stop<CapturedDrivers[i].rva+bytes);
  }
  CHECK(ARRAY_SIZE(CapturedEdges)==121&&CapturedHandleCount==466&&ARRAY_SIZE(CapturedUsbPaths)==27);
  printf("CAPTURE inputs verified: 466 handles, 121 edges, 27 USB paths; 3 exact local code lengths/SHA256.\n");fflush(stdout);
  if(!strcmp(group,"all")||!strcmp(group,"capture"))CapturedAdmission();
  if(!strcmp(group,"all")||!strcmp(group,"driver-pins"))DriverAdmission();
  if(!strcmp(group,"all")||!strcmp(group,"pin-rejections"))PinRejections();
  if(!strcmp(group,"all")||!strcmp(group,"legacy"))LegacyCases();
  if(!strcmp(group,"all")||!strcmp(group,"guards"))GuardBoundaries();
  if(!strcmp(group,"all")||!strcmp(group,"handoff"))HandoffBoundaries();
  for(UINTN i=0;i<3;i++){free(Code[i]);free(LoadedCode[i]);}
  printf("MSI group=%s PASS checks=%u; host mocks cannot qualify transitive driver Stop effects.\n",group,Checks);return 0;
}
