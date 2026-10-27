/* SPDX-License-Identifier: GPL-2.0-only */
/* Real ownership adapter, captured graph and code; all EFI effects simulated. */
#include "../x99_release.c"
#include "../format.h"
#include "cap4_graph.h"
#include "cap4_usb_paths.h"
#include "captured_config.h"
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
static unsigned Checks;static enum MODE Mode;
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"MSI FAIL mode=%d line=%d: %s\n",Mode,__LINE__,#x);exit(1);}}while(0)
GU_PLATFORM Gu;
static EFI_BOOT_SERVICES Bs;static EFI_SYSTEM_TABLE St;static EFI_PCI_IO_PROTOCOL Io;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Keys;static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Text;
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE GopMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION GopInfo;
static EFI_DRIVER_BINDING_PROTOCOL Bind[3];static EFI_LOADED_IMAGE_PROTOCOL Images[3];
static UINT8 *Code[3];static UINT32 Cfg[64];static UINT64 Attrs;
static UINTN Stage,Stops,Enables,Closes;static BOOLEAN Exclusive,Cold,Drift;
static const UINT8 UsbMousePath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,0x14,3,5,6,0,0,3,127,255,4,0};
static UINT8 BadMousePath[sizeof(UsbMousePath)];
static EDGE NewEdges[16];static UINTN NewCount;
static char Notes[1<<16];static size_t NotesUsed;
static EFI_HANDLE H(UINTN id){return (EFI_HANDLE)(id+1);}
static EFI_HANDLE Agent(UINTN i){return H(i==0?307:i==1?312:393);}
static EFI_STATUS EFIAPI KeyRead(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *p,EFI_INPUT_KEY *key){(void)p;(void)key;return EFI_NOT_READY;}
static EFI_STATUS EFIAPI KeyReset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *p,BOOLEAN verify){(void)p;(void)verify;return EFI_SUCCESS;}
/* The firmware formatter: an unsupported specifier would fault the real transaction. */
VOID GuLog(const char *f,...){
  char line[2048];va_list a;va_start(a,f);size_t n=GuFormatV(line,sizeof(line),f,a);va_end(a);
  CHECK(n!=(size_t)-1&&n<sizeof(line)&&NotesUsed+n<sizeof(Notes));memcpy(Notes+NotesUsed,line,n);NotesUsed+=n;Notes[NotesUsed]=0;
}
VOID GuLogText(const char *s){(void)s;}
BOOLEAN GuBootLogOpen(VOID){return TRUE;}
BOOLEAN GuTargetConfig(const UINT32 *c,UINTN s,UINTN b,UINTN d,UINTN f){return c[0]==GU_BOARD_ID&&c[11]==GU_BOARD_SUBSYSTEM&&!s&&b==1&&!d&&!f;}
BOOLEAN GuAd104ColdEntry(VOID){return Cold;}
BOOLEAN GuSafeToReturn(VOID){return TRUE;}
UINT32 GuRead32(UINT32 off){switch(off){case 0:return 0x194000a1;case 0x110100:return 0x10;case 0x82381c:return 0x88888888;case 0x823820:return 8;case 0x823804:return 0xffffffff;default:return 0;}}
static EFI_STATUS EFIAPI Free(VOID *p){free(p);return EFI_SUCCESS;}
static BOOLEAN Filter(const EDGE *e) {
  if(Drift&&Mode==USB_REMOVE_POINTERS&&(e->handle==H(400)||e->handle==H(408)||
     (e->handle==H(369)&&(e->controller==H(400)||e->controller==H(408)))))return TRUE;
  if(Drift&&Mode==USB_REMOVE_HOST_DRIVER&&e->handle==H(369)&&Guid(&e->protocol,&UsbHcGuid)&&e->attributes==0x10)return TRUE;
  if(Drift&&Mode==USB_REMOVE_HOST_PCI&&e->handle==H(369)&&Guid(&e->protocol,&PciGuid))return TRUE;
  if(Stage>=1&&e->handle==H(394)&&e->agent==Agent(0)&&Guid(&e->protocol,&TextGuid))return TRUE;
  if(Stage>=2&&e->handle==H(394)&&e->agent==Agent(1)&&Guid(&e->protocol,&GopGuid))return TRUE;
  return Stage>=3&&e->handle==H(365)&&e->agent==Agent(2);
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE kind,EFI_GUID *g,VOID *key,UINTN *count,EFI_HANDLE **h) {
  (void)key;*h=NULL;
  if(kind==AllHandles){*count=458;*h=malloc(*count*sizeof(**h));CHECK(*h);for(UINTN i=0;i<*count;i++)(*h)[i]=H(i);return EFI_SUCCESS;}
  CHECK(kind==ByProtocol&&Guid(g,&GopGuid));
  if(Stage>=3){*count=0;return EFI_NOT_FOUND;}
  *count=1;*h=malloc(sizeof(**h));CHECK(*h);(*h)[0]=H(394);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *g,VOID **p) {
  *p=NULL;
  if(Guid(g,&PathGuid)){if(h==H(365))*p=(VOID *)PciPath;else if(h==H(394)&&Stage<3)*p=(VOID *)GopPath;}
  if(Guid(g,&PathGuid))for(UINTN i=0;i<ARRAY_SIZE(CapturedUsbPaths);i++)
    if(h==CapturedUsbPaths[i].handle)*p=(VOID *)CapturedUsbPaths[i].path;
  if(Guid(g,&PathGuid)&&(h==H(457)||h==H(456))&&Mode>=POINTER_LATE_OK)
    *p=(VOID *)((Mode==POINTER_BAD_USB_PATH||Mode==POINTER_BAD_USB_HOST||Mode==USB_OFF_HOST_REMOVAL)?BadMousePath:UsbMousePath);
  if(Guid(g,&PathGuid)&&Mode==USB_REMOVE_RECONNECT&&Drift&&!NewCount&&(h==H(456)||h==H(457)))*p=NULL;
  if(Guid(g,&PciGuid)&&h==H(365))*p=&Io;
  if(Guid(g,&GopGuid)&&h==H(394)&&Stage<3)*p=&Gop;
  if(Guid(g,&InputGuid)&&h==H(310))*p=&Keys;
  if(Guid(g,&TextGuid)&&h==H(310))*p=&Text;
  for(UINTN i=0;i<3;i++) {
    if(Guid(g,&BindingGuid)&&h==Agent(i))*p=&Bind[i];
    if(Guid(g,&ImageGuid)&&h==Bind[i].ImageHandle)*p=&Images[i];
  }
  return *p?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Protocols(EFI_HANDLE h,EFI_GUID ***out,UINTN *n) {
  EFI_GUID *unique[64];*n=0;
  for(UINTN i=0;i<ARRAY_SIZE(CapturedEdges);i++)if(CapturedEdges[i].handle==h&&!Filter(&CapturedEdges[i])) {
    BOOLEAN seen=FALSE;for(UINTN j=0;j<*n;j++)if(Guid(unique[j],&CapturedEdges[i].protocol))seen=TRUE;
    if(!seen)unique[(*n)++]=(EFI_GUID *)&CapturedEdges[i].protocol;
  }
  if(Exclusive&&h==H(365)){BOOLEAN seen=FALSE;for(UINTN j=0;j<*n;j++)if(Guid(unique[j],&PciGuid))seen=TRUE;if(!seen)unique[(*n)++]=&PciGuid;}
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
    const EDGE *e=&CapturedEdges[i];if(e->handle!=h||!Guid(g,&e->protocol)||Filter(e))continue;
    if(Mode==UNRELATED_EDGE_LOST&&Stage&&i==0)continue;
    (*out)[(*n)++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){e->agent,e->controller,e->attributes,e->count};
  }
  if(Exclusive&&h==H(365)&&Guid(g,&PciGuid))(*out)[(*n)++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){H(450),NULL,EFI_OPEN_PROTOCOL_EXCLUSIVE,1};
  if(Mode==EXTRA_GPU_OWNER&&h==H(365)&&Guid(g,&PciGuid))(*out)[(*n)++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){H(449),H(365),0x10,1};
  if(Mode==GRAPH_DRIFT&&Drift&&h==H(365)&&Guid(g,&PciGuid))(*out)[(*n)++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){H(448),H(365),0x10,1};
  for(UINTN i=0;i<NewCount;i++)if(NewEdges[i].handle==h&&Guid(g,&NewEdges[i].protocol)) {
    EDGE *e=&NewEdges[i];(*out)[(*n)++]=(EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){e->agent,e->controller,e->attributes,e->count};
  }
  if(Mode==PROTECTED_COUNT_CHANGE&&Drift&&h==H(365)&&Guid(g,&PciGuid)&&*n)(*out)[0].OpenCount++;
  CHECK(*n<=128);return EFI_SUCCESS;
}
static EFI_TPL EFIAPI Raise(EFI_TPL t){CHECK(t==TPL_NOTIFY);return TPL_APPLICATION;}
static VOID EFIAPI Restore(EFI_TPL t){CHECK(t==TPL_APPLICATION);}
static EFI_STATUS EFIAPI Location(EFI_PCI_IO_PROTOCOL *p,UINTN *s,UINTN *b,UINTN *d,UINTN *f){CHECK(p==&Io);*s=*d=*f=0;*b=1;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI ConfigRead(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN count,VOID *out){CHECK(p==&Io&&w==EfiPciIoWidthUint32&&!off&&count==64);memcpy(out,Cfg,sizeof(Cfg));return EFI_SUCCESS;}
static EFI_STATUS EFIAPI GetBar(EFI_PCI_IO_PROTOCOL *p,UINT8 index,UINT64 *support,VOID **out){
  CHECK(p==&Io&&!index);*support=0;EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=calloc(1,sizeof(*d));CHECK(d);d->Desc=0x8a;d->Len=43;d->AddrRangeMin=GU_BAR0_BASE;d->AddrRangeMax=0x80ffffff;d->AddrLen=0x1000000;*out=d;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Attributes(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 mask,UINT64 *out){
  CHECK(p==&Io);
  if(op==EfiPciIoAttributeOperationGet){CHECK(out&&!mask);*out=Attrs;return EFI_SUCCESS;}
  if(op==EfiPciIoAttributeOperationSupported){CHECK(out&&!mask);*out=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;return EFI_SUCCESS;}
  CHECK(op==EfiPciIoAttributeOperationEnable&&mask==(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY)&&!out&&Exclusive);
  Enables++;if(Mode==ATTR_FAIL)return EFI_DEVICE_ERROR;
  Attrs|=mask;Cfg[1]|=3;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_HANDLE h,EFI_GUID *g,VOID **out,EFI_HANDLE a,EFI_HANDLE controller,UINT32 attrs){
  CHECK(h==H(365)&&Guid(g,&PciGuid)&&a==H(450)&&!controller&&attrs==EFI_OPEN_PROTOCOL_EXCLUSIVE&&Stage==3&&!Exclusive);
  if(Mode==CLAIM_FAIL)return EFI_ACCESS_DENIED;Exclusive=TRUE;*out=&Io;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_HANDLE h,EFI_GUID *g,EFI_HANDLE a,EFI_HANDLE controller){
  CHECK(h==H(365)&&Guid(g,&PciGuid)&&a==H(450)&&!controller&&Exclusive);Closes++;
  if(Mode==CLOSE_FAIL)return EFI_DEVICE_ERROR;Exclusive=FALSE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Disconnect(EFI_HANDLE controller,EFI_HANDLE driver,EFI_HANDLE child){
  CHECK(!child&&Stops<3);CHECK(controller==(Stops==2?H(365):H(394))&&driver==Agent(Stops));Stops++;
  Stage=Stops;
  if(Stage==3){Cfg[1]&=~3u;Attrs=0;if(Mode==DECODE_EXTRA_BIT)Cfg[2]^=1;}
  if(Mode==INPUT_CHANGED&&Stage==1)St.ConIn=NULL;
  if(Mode==COLD_CHANGED&&Stage==3)Cold=FALSE;
  if((Mode==STOP1_FAIL&&Stage==1)||(Mode==STOP2_FAIL&&Stage==2)||(Mode==STOP3_FAIL&&Stage==3))return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
static void Setup(void) {
  memset(&Gu,0,sizeof(Gu));memset(&Plan,0,sizeof(Plan));memset(&Bs,0,sizeof(Bs));memset(&St,0,sizeof(St));memset(&Io,0,sizeof(Io));
  memset(&Keys,0,sizeof(Keys));memcpy(Cfg,CapturedConfig,sizeof(Cfg));Stage=Stops=Enables=Closes=0;Exclusive=Drift=FALSE;Cold=TRUE;
  NotesUsed=0;Notes[0]=0;NewCount=0;memcpy(BadMousePath,UsbMousePath,sizeof(UsbMousePath));
  Bs.LocateHandleBuffer=Locate;Bs.HandleProtocol=Handle;Bs.ProtocolsPerHandle=Protocols;Bs.OpenProtocolInformation=Opens;Bs.FreePool=Free;
  Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.OpenProtocol=Open;Bs.CloseProtocol=Close;Bs.DisconnectController=Disconnect;
  St.BootServices=&Bs;St.ConsoleInHandle=St.ConsoleOutHandle=St.StandardErrorHandle=H(310);St.ConIn=&Keys;St.ConOut=St.StdErr=&Text;
  Keys.ReadKeyStroke=KeyRead;Keys.Reset=KeyReset;Keys.WaitForKey=(EFI_EVENT)1;
  Gop.Mode=&GopMode;GopMode.Info=&GopInfo;GopMode.SizeOfInfo=sizeof(GopInfo);GopMode.FrameBufferBase=0x4000000000;GopMode.FrameBufferSize=0x1680000;
  Io.Pci.Read=ConfigRead;Io.GetLocation=Location;Io.GetBarAttributes=GetBar;Io.Attributes=Attributes;
  Cfg[0]=GU_BOARD_ID;Cfg[1]=0x00100003;Cfg[2]=0x030000a1;Cfg[11]=GU_BOARD_SUBSYSTEM;
  Attrs=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
  Gu.bs=&Bs;Gu.pci=&Io;Gu.image=H(450);Gu.handle=H(365);Gu.profile_verified=Gu.prom_verified=TRUE;
  for(UINTN i=0;i<3;i++) {
    memset(&Bind[i],0,sizeof(Bind[i]));memset(&Images[i],0,sizeof(Images[i]));
    Bind[i].DriverBindingHandle=Agent(i);Bind[i].ImageHandle=i==0?H(306):i==1?H(311):Agent(i);Bind[i].Version=i==2?0x7000b:0x10;
    Bind[i].Stop=(EFI_DRIVER_BINDING_STOP)(UINTN)(Code[i]+(i==0?0x126c:i==1?0xa38:0x6ec));
    Images[i].ImageBase=Code[i];Images[i].ImageSize=i==0?24576:i==1?14816:162016;Images[i].ImageCodeType=EfiBootServicesCode;
    Images[i].DeviceHandle=i==2?H(365):H(5);Images[i].FilePath=(VOID *)(i==0?OutFile:i==1?GfxFile:NvFile);
  }
  if(Mode==WRONG_DRIVER)Bind[1].Version++;
  if(Mode==USB_REMOVE_RECONNECT||Mode==USB_DYNAMIC_COUNT||Mode==USB_REMOVE_INPUT_CHANGED) {
    EFI_GUID usbio={0x2b2f68d6,0x0cd2,0x44cf,{0x8e,0x8b,0xbb,0xa2,0x0b,0x1b,0x5b,0x75}};
    EFI_GUID keyex={0xdd9e7534,0x7762,0x4698,{0x8c,0x14,0xf5,0x85,0x17,0xa6,0x25,0xaa}};
    NewCount=9;
    NewEdges[0]=(EDGE){H(369),H(83),H(457),UsbHcGuid,8,1};
    NewEdges[1]=(EDGE){H(457),H(84),H(457),usbio,0x10,1};
    NewEdges[2]=(EDGE){H(457),H(309),H(457),PointerGuid,0x10,1};
    NewEdges[3]=(EDGE){H(457),H(309),H(310),PointerGuid,8,1};
    NewEdges[4]=(EDGE){H(369),H(83),H(456),UsbHcGuid,8,1};
    NewEdges[5]=(EDGE){H(456),H(84),H(456),usbio,0x10,1};
    NewEdges[6]=(EDGE){H(456),H(308),H(456),InputGuid,0x10,1};
    NewEdges[7]=(EDGE){H(456),H(308),H(456),keyex,0x10,1};
    NewEdges[8]=(EDGE){H(456),H(308),H(310),InputGuid,8,1};
  }
  if(Mode==USB_OFF_HOST_REMOVAL) {
    BadMousePath[17]=1;NewCount=1;NewEdges[0]=(EDGE){H(369),H(83),H(457),UsbHcGuid,8,1};
  }
}
int main(int argc,char **argv) {
  CHECK(argc==2);
  for(UINTN i=0;i<3;i++) {
    char name[4096];snprintf(name,sizeof(name),"%s/driver-%u.bin",argv[1],i==0?307:i==1?312:393);
    FILE *f=fopen(name,"rb");CHECK(f);UINTN size=i==0?24576:i==1?14816:162016,rva=i==2?0x280:0x2a0,bytes=i==0?19872:i==1?7280:137711;
    Code[i]=calloc(1,size);CHECK(Code[i]);CHECK(fread(Code[i]+rva,1,bytes,f)==bytes&&fgetc(f)==EOF);fclose(f);
  }
  for(Mode=OK;Mode<MODE_COUNT;Mode++) {
    Setup();EFI_STATUS s=GuX99Preflight(&St,H(450),H(365));
    if(Mode==WRONG_DRIVER||Mode==EXTRA_GPU_OWNER){CHECK(s!=EFI_SUCCESS&&!Stops&&!GuX99Mutated());continue;}
    CHECK(s==EFI_SUCCESS);s=GuX99Release();
    if(Mode>=USB_REMOVE_RECONNECT) {
      CHECK(s==EFI_SUCCESS&&GuX99Released()&&Plan.usb_host==H(369)&&Plan.usb_agent==H(83));
      Drift=TRUE;
      if(Mode==USB_REMOVE_RECONNECT||Mode==USB_REMOVE_INPUT_CHANGED) {
        CHECK(Original.count==165);NewCount=0;CHECK(Plan.key_agent==H(308));
      }
      if(Mode==USB_DYNAMIC_COUNT)NewEdges[0].count=2;
      if(Mode==USB_OFF_HOST_REMOVAL)NewCount=0;
      if(Mode==USB_REMOVE_INPUT_CHANGED)St.ConIn=NULL;
      BOOLEAN okay=Mode==USB_REMOVE_RECONNECT||Mode==USB_REMOVE_POINTERS;
      if(okay) {
        CHECK(GuX99Released());CHECK(strstr(Notes,"original USB relationships absent"));
        if(Mode==USB_REMOVE_RECONNECT) {
          /* RUN0006: 165 original -> 152 current, with all 9 USB edges gone. */
          CHECK(Current.count==152);NewCount=9;CHECK(GuX99Released());NewCount=0;CHECK(GuX99Released());
        }
        Gu.quiesced=TRUE;Gu.flrs=1;CHECK(GuX99Finish()==EFI_SUCCESS&&!Exclusive);CHECK(GuX99ReadyForWindows());
      } else {
        CHECK(!GuX99Released());Gu.quiesced=TRUE;Gu.flrs=1;
        CHECK(GuX99Finish()!=EFI_SUCCESS&&Exclusive);CHECK(!GuX99ReadyForWindows());
      }
      printf("MSI USB lifecycle case %d PASS\n",Mode);continue;
    }
    if(Mode>=POINTER_LATE_OK) {
      CHECK(s==EFI_SUCCESS&&GuX99Released()&&Plan.pointer_agent==H(309));
      NewCount=2;
      NewEdges[0]=(EDGE){H(457),H(309),H(457),PointerGuid,EFI_OPEN_PROTOCOL_BY_DRIVER,1};
      NewEdges[1]=(EDGE){H(457),H(309),H(310),PointerGuid,EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER,1};
      if(Mode==POINTER_WRONG_AGENT){NewEdges[0].agent=NewEdges[1].agent=H(439);}
      if(Mode==POINTER_NO_OWNER){NewCount=1;NewEdges[0]=NewEdges[1];}
      if(Mode==POINTER_DUP_OWNER){NewCount=3;NewEdges[2]=NewEdges[0];}
      if(Mode==POINTER_GPU_HANDLE)NewEdges[1].handle=H(365);
      if(Mode==POINTER_GPU_CONTROLLER)NewEdges[1].controller=H(365);
      if(Mode==POINTER_WRONG_GUID)NewEdges[1].protocol.Data2++;
      if(Mode==POINTER_BAD_COUNT)NewEdges[1].count++;
      if(Mode==POINTER_BAD_USB_PATH)BadMousePath[19]=4;
      if(Mode==POINTER_BAD_USB_HOST)BadMousePath[17]=1;
      if(Mode==POINTER_CHANGED_INPUT)St.ConIn=NULL;
      if(Mode==POINTER_LATE_OK) {
        CHECK(GuX99Released());CHECK(strstr(Notes,"verified USB pointer"));
        Gu.quiesced=TRUE;Gu.flrs=1;CHECK(GuX99Finish()==EFI_SUCCESS&&!Exclusive);
        CHECK(GuX99ReadyForWindows());
      } else {
        CHECK(!GuX99Released());Gu.quiesced=TRUE;Gu.flrs=1;
        CHECK(GuX99Finish()!=EFI_SUCCESS&&Exclusive);CHECK(!GuX99ReadyForWindows());
      }
      printf("MSI late pointer case %d PASS\n",Mode);continue;
    }
    if(Mode>=USB_ADDITIONS) {
      CHECK(s==EFI_SUCCESS&&GuX99Released());
      EFI_GUID usb={0x3e745226,0x9818,0x45b6,{0xa2,0xac,0xd7,0xcd,0x0e,0x8b,0xa2,0xbc}};
      Drift=TRUE;NewCount=1;NewEdges[0]=(EDGE){H(457),H(440),H(441),usb,8,1};
      if(Mode==USB_TO_GPU)NewEdges[0].controller=H(365);
      if(Mode==USB_GPU_AGENT)NewEdges[0].agent=H(365);
      if(Mode==USB_GPU_HANDLE)NewEdges[0].handle=H(365);
      if(Mode==USB_TO_CONSOLE)NewEdges[0].controller=H(310);
      if(Mode==USB_DRIVER_AGENT)NewEdges[0].agent=Agent(2);
      if(Mode==NEW_EDGE_DUPLICATE){NewCount=2;NewEdges[1]=NewEdges[0];}
      if(Mode==OWN_WRONG_PROTOCOL)NewEdges[0]=(EDGE){H(365),H(450),NULL,usb,EFI_OPEN_PROTOCOL_EXCLUSIVE,1};
      if(Mode==PROTECTED_COUNT_CHANGE)NewCount=0;
      if(Mode==USB_ADDITIONS) {
        /* Unrelated USB additions, alongside the exclusive GPU claim. */
        NewCount=5;for(UINTN k=1;k<NewCount;k++){NewEdges[k]=NewEdges[0];NewEdges[k].controller=H(441+k);}
        CHECK(GuX99Released());Gu.quiesced=TRUE;Gu.flrs=1;
        CHECK(GuX99Finish()==EFI_SUCCESS&&!Exclusive);CHECK(GuX99ReadyForWindows());
      } else {
        CHECK(!GuX99Released());Gu.quiesced=TRUE;Gu.flrs=1;
        CHECK(GuX99Finish()!=EFI_SUCCESS&&Exclusive);CHECK(!GuX99ReadyForWindows());
      }
      printf("MSI enumeration case %d PASS\n",Mode);continue;
    }
    if(Mode>=STATUS_DRIFT) {
      CHECK(s==EFI_SUCCESS&&Stops==3&&Exclusive&&GuX99Released());
      UINT32 *devsta=&Cfg[0x80/4],*linksta=&Cfg[0x88/4];
      CHECK(Cfg[0x78/4]==0x0012b410&&*devsta==0x00092930&&*linksta==0x11030040);
      /* Pre-FLR drift, as the firmware run produced before the aperture check. */
      Drift=TRUE;NotesUsed=0;Notes[0]=0;
      if(Mode==STATUS_DRIFT){Cfg[1]|=0x00080000;*devsta=(*devsta|0x00020000)&~0x00010000u;*linksta|=0x40000000;}
      if(Mode==FATAL_DRIFT)*devsta|=0x00040000;
      if(Mode==PARITY_DRIFT)Cfg[1]|=0x80000000;
      if(Mode==CONTROL_DRIFT)*devsta^=0x00000010;
      if(Mode==LINK_DRIFT)*linksta=(*linksta&~0x000f0000u)|0x00010000;
      if(Mode==COMMAND_DRIFT)Cfg[1]|=4;
      if(Mode==STATUS_DRIFT) {
        CHECK(!GuX99Released()&&strstr(Notes,"PCI config drift refused: offset=0x4"));
        memcpy(Cfg,Plan.config,sizeof(Cfg));*devsta&=~0x00090000u;
        CHECK(!GuX99Released()); /* Even preexisting bits may clear only after FLR. */
        Gu.quiesced=TRUE;Gu.flrs=1;NotesUsed=0;Notes[0]=0;
        CHECK(GuX99Released());CHECK(strstr(Notes,"PCI status drift tolerated: offset=0x80"));
        CHECK(GuX99Finish()==EFI_SUCCESS&&!Exclusive&&Closes==1);CHECK(GuX99ReadyForWindows());
      } else {
        const char *why=Mode==COMMAND_DRIFT?"check failed: config-read":Mode==GRAPH_DRIFT?"check failed: graph":"check failed: config-drift";
        CHECK(!GuX99Released()&&strstr(Notes,why));
        if(Mode==FATAL_DRIFT)CHECK(strstr(Notes,"refused: offset=0x80 captured=0x92930 now=0xd2930 bits=0x40000 flrs=0"));
        if(Mode==PARITY_DRIFT)CHECK(strstr(Notes,"refused: offset=0x4 captured=0x100003 now=0x80100003 bits=0x80000000"));
        if(Mode==CONTROL_DRIFT)CHECK(strstr(Notes,"refused: offset=0x80 captured=0x92930 now=0x92920 bits=0x10"));
        if(Mode==LINK_DRIFT)CHECK(strstr(Notes,"refused: offset=0x88 captured=0x11030040 now=0x11010040 bits=0x20000"));
        if(Mode==GRAPH_DRIFT)CHECK(strstr(Notes,"MSI graph unexpected: handle=0x16e agent=0x1c1 controller=0x16e protocol="));
        Gu.quiesced=TRUE;Gu.flrs=1;CHECK(GuX99Finish()!=EFI_SUCCESS&&Exclusive&&!Closes&&strstr(Notes,"MSI finish refused"));
        CHECK(!GuX99ReadyForWindows());
      }
      printf("MSI drift case %d PASS\n",Mode);
      continue;
    }
    if(Mode==OK||Mode==CLOSE_FAIL) {
      CHECK(s==EFI_SUCCESS&&Stops==3&&Enables==1&&Exclusive&&Gu.owned&&GuX99Released());
      Gu.quiesced=TRUE;Gu.flrs=1;s=GuX99Finish();
      if(Mode==OK){CHECK(s==EFI_SUCCESS&&!Exclusive&&!Gu.owned&&Closes==1);CHECK(GuX99ReadyForWindows());Gu.fault=TRUE;CHECK(!GuX99ReadyForWindows());Gu.fault=FALSE;Stage=2;CHECK(!GuX99ReadyForWindows());Stage=3;}
      else CHECK(s!=EFI_SUCCESS&&Exclusive&&Closes==1);
    } else {CHECK(s!=EFI_SUCCESS&&GuX99Mutated());CHECK(Stops<=3);if(Mode==DECODE_EXTRA_BIT||Mode==CLAIM_FAIL)CHECK(!Enables);}
    printf("MSI release case %d PASS stops=%zu decode_writes=%zu\n",Mode,(size_t)Stops,(size_t)Enables);
  }
  for(UINTN i=0;i<3;i++)free(Code[i]);printf("MSI: %u checks across %u cases; captured graph/code, all callbacks simulated.\n",Checks,MODE_COUNT);return 0;
}
