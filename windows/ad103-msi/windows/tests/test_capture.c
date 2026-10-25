/* SPDX-License-Identifier: GPL-2.0-only */
/* Synthetic EFI metadata only. No firmware, GPU, disk or private capture input. */
#include "capture.h"
#include "app_io.h"
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define H(n) ((EFI_HANDLE)(UINTN)(n))
#define GPU H(0x100)
#define DISPLAY H(0x200)
#define EDID H(0x300)
#define CONSOLE H(0x400)
#define NVIDIA H(0x500)
#define GRAPHICS H(0x600)
#define TEXT H(0x700)
#define IMAGE_BYTES 4096
#define OPT 0x98
#define SECTIONS (OPT+240)

GU_PLATFORM Gu;
static EFI_BOOT_SERVICES bs;
static EFI_SYSTEM_TABLE table;
static EFI_GUID pci_guid=EFI_PCI_IO_PROTOCOL_GUID,path_guid=EFI_DEVICE_PATH_PROTOCOL_GUID,
  gop_guid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,text_guid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID,
  input_guid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID,binding_guid=EFI_DRIVER_BINDING_PROTOCOL_GUID,
  image_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID edid_guid={0x1c0c34f6,0xd380,0x41fa,{0xa0,0x49,0x8a,0xd0,0x6c,0x1a,0x66,0xaa}};
static EFI_GUID unknown_guid={0x12345678,0x1234,0x5678,{0x90,0xab,0xcd,0xef,0x01,0x23,0x45,0x67}};
static struct {
  UINT8 images[3][IMAGE_BYTES];
  EFI_DRIVER_BINDING_PROTOCOL bindings[3];
  EFI_LOADED_IMAGE_PROTOCOL loaded[3];
  UINT8 paths[4][16];
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL output;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL input;
} arena;
static const char *scenario;
static char capture[262144];
static size_t capture_bytes;
static unsigned allocated,freed,allocation_attempts,snapshots,mutations,opened,closed,writes;

static BOOLEAN is(const char *name){return !strcmp(scenario,name);}
static BOOLEAN eq(const EFI_GUID *a,const EFI_GUID *b){return !memcmp(a,b,sizeof(*a));}
static VOID *pool(UINTN bytes){
  allocation_attempts++;
  if((is("allocation-first")&&allocation_attempts==1)||(is("allocation-map")&&bytes==262144))return NULL;
  VOID *value=calloc(1,bytes?bytes:1);assert(value);allocated++;return value;
}
VOID *GuAlloc(size_t bytes){return pool(bytes);}
static EFI_STATUS EFIAPI free_pool(VOID *value){assert(value);freed++;free(value);return is("free-error")?EFI_DEVICE_ERROR:EFI_SUCCESS;}

EFI_STATUS GuCaptureOpen(VOID){opened++;return is("open-error")?EFI_DEVICE_ERROR:EFI_SUCCESS;}
EFI_STATUS GuCaptureWrite(const char *text){
  writes++;
  if(is("write-error")&&writes==5)return EFI_DEVICE_ERROR;
  size_t count=strlen(text);assert(capture_bytes+count<sizeof(capture));
  memcpy(capture+capture_bytes,text,count+1);capture_bytes+=count;return EFI_SUCCESS;
}
EFI_STATUS GuCaptureClose(VOID){closed++;return is("close-error")?EFI_DEVICE_ERROR:EFI_SUCCESS;}

static EFI_STATUS EFIAPI disconnect(EFI_HANDLE a,EFI_HANDLE b,EFI_HANDLE c){
  (void)a;(void)b;(void)c;mutations++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI connect(EFI_HANDLE a,EFI_HANDLE *b,EFI_DEVICE_PATH_PROTOCOL *c,BOOLEAN d){
  (void)a;(void)b;(void)c;(void)d;mutations++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI open_protocol(EFI_HANDLE a,EFI_GUID *b,VOID **c,EFI_HANDLE d,EFI_HANDLE e,UINT32 f){
  (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;mutations++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI text_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *a,BOOLEAN b){(void)a;(void)b;mutations++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI text_output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *a,CHAR16 *b){(void)a;(void)b;mutations++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI text_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *a,UINTN b){(void)a;(void)b;mutations++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI input_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *a,BOOLEAN b){(void)a;(void)b;mutations++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI input_read(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *a,EFI_INPUT_KEY *b){(void)a;(void)b;mutations++;return EFI_DEVICE_ERROR;}

static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *count,EFI_HANDLE **out){
  (void)guid;(void)key;assert(type==AllHandles);snapshots++;*count=0;*out=NULL;
  if(is("locate-error"))return EFI_DEVICE_ERROR;
  if(is("null-handles")){*count=7;return EFI_SUCCESS;}
  EFI_HANDLE values[]={GPU,DISPLAY,EDID,CONSOLE,NVIDIA,GRAPHICS,TEXT};
  *out=pool(sizeof(values));memcpy(*out,values,sizeof(values));*count=7;
  if(is("too-many-handles"))*count=4097;
  if(is("duplicate-handle"))(*out)[6]=(*out)[0];
  if(is("null-handle"))(*out)[6]=NULL;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI protocols(EFI_HANDLE handle,EFI_GUID ***out,UINTN *count){
  EFI_GUID *list[8];UINTN n=0;*out=NULL;*count=0;
  if(handle==GPU){list[n++]=&pci_guid;list[n++]=&path_guid;if(is("unknown-edge"))list[n++]=&unknown_guid;}
  else if(handle==DISPLAY){list[n++]=&gop_guid;list[n++]=&text_guid;list[n++]=&path_guid;}
  else if(handle==EDID){list[n++]=&edid_guid;list[n++]=&path_guid;}
  else if(handle==CONSOLE){list[n++]=&text_guid;list[n++]=&input_guid;}
  else if(handle==NVIDIA||handle==GRAPHICS||handle==TEXT){list[n++]=&binding_guid;list[n++]=&image_guid;}
  else return EFI_NOT_FOUND;
  if(handle==GPU&&is("protocol-error"))return EFI_DEVICE_ERROR;
  if(handle==GPU&&is("null-protocols")){*count=n;return EFI_SUCCESS;}
  *out=pool(n*sizeof(**out));memcpy(*out,list,n*sizeof(**out));*count=n;
  if(handle==GPU&&is("too-many-protocols"))*count=65;
  if(handle==GPU&&is("duplicate-protocol"))(*out)[1]=(*out)[0];
  if(handle==GPU&&is("null-protocol"))(*out)[1]=NULL;
  return EFI_SUCCESS;
}
static EFI_OPEN_PROTOCOL_INFORMATION_ENTRY entry(EFI_HANDLE agent,EFI_HANDLE controller,UINT32 attributes,UINT32 count){
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY e={agent,controller,attributes,count};return e;
}
static EFI_STATUS EFIAPI information(EFI_HANDLE handle,EFI_GUID *guid,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *count){
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY entries[3];UINTN n=0;*out=NULL;*count=0;
  if(handle==GPU&&eq(guid,&pci_guid))entries[n++]=entry(NVIDIA,GPU,0x10,is("drift")&&snapshots==2?2:1);
  if(handle==GPU&&eq(guid,&path_guid)){
    entries[n++]=entry(NVIDIA,DISPLAY,8,1);entries[n++]=entry(NVIDIA,is("duplicate-edge")?DISPLAY:EDID,8,1);
  }
  if(handle==GPU&&eq(guid,&unknown_guid))entries[n++]=entry(NVIDIA,EDID,0x80,1);
  if(handle==DISPLAY&&eq(guid,&gop_guid))entries[n++]=entry(GRAPHICS,DISPLAY,0x10,1);
  if(handle==DISPLAY&&eq(guid,&text_guid)){
    entries[n++]=entry(TEXT,DISPLAY,0x10,1);entries[n++]=entry(TEXT,CONSOLE,8,1);
  }
  if(handle==EDID&&eq(guid,&path_guid))entries[n++]=entry(NVIDIA,EDID,2,1);
  if(handle==GPU&&eq(guid,&pci_guid)){
    if(is("open-info-error"))return EFI_DEVICE_ERROR;
    if(is("null-opens")){*count=1;return EFI_SUCCESS;}
    if(is("zero-open-count"))entries[0].OpenCount=0;
  }
  if(n){*out=pool(n*sizeof(**out));memcpy(*out,entries,n*sizeof(**out));*count=n;}
  if(handle==GPU&&eq(guid,&pci_guid)&&is("too-many-opens"))*count=129;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI interface(EFI_HANDLE handle,EFI_GUID *guid,VOID **out){
  *out=NULL;
  if(eq(guid,&path_guid)){
    if(handle==CONSOLE)return EFI_UNSUPPORTED;
    unsigned which=handle==GPU?0:handle==DISPLAY?1:handle==EDID?2:3;
    *out=arena.paths[which];
    if(handle==EDID&&is("unreadable-path"))*out=(VOID*)0x1000;
    if(handle==EDID&&is("path-error")){*out=NULL;return EFI_DEVICE_ERROR;}
    return EFI_SUCCESS;
  }
  unsigned index=handle==NVIDIA?0:handle==GRAPHICS?1:handle==TEXT?2:3;
  if(index==3)return EFI_UNSUPPORTED;
  if(eq(guid,&binding_guid)){
    if(index==0&&is("binding-error"))return EFI_DEVICE_ERROR;
    *out=index==0&&is("unreadable-binding")?(VOID*)0x1000:&arena.bindings[index];return EFI_SUCCESS;
  }
  if(eq(guid,&image_guid)){
    if(index==0&&is("image-error"))return EFI_DEVICE_ERROR;
    *out=index==0&&is("unreadable-image-interface")?(VOID*)0x1000:&arena.loaded[index];return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI locate_path(EFI_GUID *guid,EFI_DEVICE_PATH_PROTOCOL **path,EFI_HANDLE *handle){
  assert(eq(guid,&pci_guid));assert(*path);*handle=GPU;
  return is("parent-error")?EFI_NOT_FOUND:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI memory_map(UINTN *bytes,EFI_MEMORY_DESCRIPTOR *map,UINTN *key,UINTN *stride,UINT32 *version){
  if(is("map-error"))return EFI_DEVICE_ERROR;
  EFI_MEMORY_DESCRIPTOR d;memset(&d,0,sizeof(d));d.Type=EfiBootServicesData;
  d.PhysicalStart=(UINTN)&arena&~(UINT64)4095;
  UINT64 end=((UINTN)&arena+sizeof(arena)+4095)&~(UINT64)4095;
  d.NumberOfPages=(end-d.PhysicalStart)/4096;
  d.Attribute=is("read-protected")?EFI_MEMORY_RP:0;
  memcpy(map,&d,sizeof(d));*bytes=sizeof(d);*stride=sizeof(d);*version=1;*key=1;
  if(is("overlapping-map")){memcpy((UINT8*)map+sizeof(d),&d,sizeof(d));*bytes*=2;}
  if(is("bad-map-stride"))*stride=1;
  return EFI_SUCCESS;
}
static void p16(UINT8 *p,UINT16 value){p[0]=(UINT8)value;p[1]=(UINT8)(value>>8);}
static void p32(UINT8 *p,UINT32 value){p16(p,(UINT16)value);p16(p+2,(UINT16)(value>>16));}
static void p64(UINT8 *p,UINT64 value){p32(p,(UINT32)value);p32(p+4,(UINT32)(value>>32));}
static void pe(UINT8 *p){
  p[0]='M';p[1]='Z';p32(p+60,0x80);memcpy(p+0x80,"PE\0\0",4);
  p16(p+0x84,0x8664);p16(p+0x86,1);p16(p+0x94,240);p16(p+OPT,0x20b);
  p32(p+OPT+16,0x220);p64(p+OPT+24,(UINTN)p);p32(p+OPT+56,IMAGE_BYTES);
  p32(p+OPT+60,0x200);p32(p+OPT+108,6);memcpy(p+SECTIONS,".text",5);
  p32(p+SECTIONS+8,0x200);p32(p+SECTIONS+12,0x200);p32(p+SECTIONS+16,0x200);
  p32(p+SECTIONS+36,0x60000020);memset(p+0x200,0x5a,0x200);
}
static void setup(const char *name){
  scenario=name;memset(&Gu,0,sizeof(Gu));memset(&bs,0,sizeof(bs));memset(&table,0,sizeof(table));
  memset(&arena,0,sizeof(arena));memset(capture,0,sizeof(capture));capture_bytes=0;
  allocated=freed=allocation_attempts=snapshots=mutations=opened=closed=writes=0;
  bs.FreePool=free_pool;bs.LocateHandleBuffer=locate;bs.ProtocolsPerHandle=protocols;
  bs.OpenProtocolInformation=information;bs.HandleProtocol=interface;bs.LocateDevicePath=locate_path;
  bs.GetMemoryMap=memory_map;bs.DisconnectController=disconnect;bs.ConnectController=connect;bs.OpenProtocol=open_protocol;
  table.BootServices=&bs;table.ConsoleOutHandle=table.ConsoleInHandle=table.StandardErrorHandle=CONSOLE;
  table.ConOut=table.StdErr=&arena.output;table.ConIn=&arena.input;
  arena.output.Reset=text_reset;arena.output.OutputString=text_output;arena.output.SetMode=text_mode;
  arena.input.Reset=input_reset;arena.input.ReadKeyStroke=input_read;arena.input.WaitForKey=(VOID*)0x800;
  for(unsigned i=0;i<4;i++){
    UINT8 *p=arena.paths[i];p[0]=1;p[1]=1;p[2]=6;p[4]=(UINT8)i;p[6]=127;p[7]=255;p[8]=4;
  }
  EFI_HANDLE drivers[]={NVIDIA,GRAPHICS,TEXT};
  for(unsigned i=0;i<3;i++){
    pe(arena.images[i]);arena.bindings[i].ImageHandle=drivers[i];arena.bindings[i].DriverBindingHandle=drivers[i];
    arena.bindings[i].Version=0x10;arena.bindings[i].Stop=(EFI_DRIVER_BINDING_STOP)(VOID*)(arena.images[i]+0x220);
    arena.bindings[i].Start=(EFI_DRIVER_BINDING_START)(VOID*)(arena.images[i]+0x230);
    arena.bindings[i].Supported=(EFI_DRIVER_BINDING_SUPPORTED)(VOID*)(arena.images[i]+0x240);
    arena.loaded[i].ImageBase=arena.images[i];arena.loaded[i].ImageSize=IMAGE_BYTES;
    arena.loaded[i].ImageCodeType=EfiBootServicesCode;arena.loaded[i].ImageDataType=EfiBootServicesData;
    arena.loaded[i].DeviceHandle=GPU;arena.loaded[i].FilePath=(VOID*)arena.paths[3];
  }
  Gu.bs=&bs;Gu.handle=GPU;Gu.image=H(0x900);
  if(is("unreadable-image"))arena.loaded[0].ImageBase=(VOID*)0x1000;
  if(is("oversize-image"))arena.loaded[0].ImageSize=16U*1024U*1024U+1;
  if(is("bad-pe"))arena.images[0][0]=0;
  if(is("bad-stop"))arena.bindings[0].Stop=(EFI_DRIVER_BINDING_STOP)(VOID*)0x1000;
  if(is("bad-path"))arena.paths[2][2]=0;
  if(is("no-memory-map"))bs.GetMemoryMap=NULL;
  if(is("owned"))Gu.owned=TRUE;
  if(is("no-locate-service"))bs.LocateHandleBuffer=NULL;
}
static void contains(const char *text){if(!strstr(capture,text)){fprintf(stderr,"%s missing: %s\n%s\n",scenario,text,capture);abort();}}
static EFI_STATUS run(const char *name){
  setup(name);UINT8 saved[sizeof(arena)];memcpy(saved,&arena,sizeof(arena));
  EFI_STATUS status=GuCaptureFirmware(&table);
  assert(allocated==freed);assert(mutations==0);
  assert(!Gu.mmio_writes&&!Gu.started&&!Gu.armed&&Gu.owned==is("owned"));
  assert(!memcmp(saved,&arena,sizeof(arena)));assert(!strstr(capture,"unlock_authorized=1"));
  if(opened&&!is("open-error"))assert(closed==1);
  return status;
}
int main(void){
  assert(run("success")==EFI_SUCCESS);assert(snapshots==2);
  contains("CAPTURE_FORMAT version=1 diagnostic=0.4.2 read_only=1 unlock_authorized=0");
  contains("ENDPOINT_PATH handle=0x300 status=0 present=1");
  contains("PATH_BYTES kind=endpoint handle=0x300");
  contains("ENDPOINT_PCI handle=0x300 status=0 parent=0x100 target=0x100");
  contains("PE agent=0x500 status=0 stop_offset=0x220 canonical_valid=1");
  contains("PE agent=0x600 status=0 stop_offset=0x220 canonical_valid=1");
  contains("PE agent=0x700 status=0 stop_offset=0x220 canonical_valid=1");
  contains("stable_active_graph=1 endpoints=4 drivers=3 gpu_writes=0 unlock_authorized=0");
  assert(run("unknown-edge")==EFI_SUCCESS);contains("attributes=0x80 count=1");
  contains("value=12345678-1234-5678-90ab-cdef01234567");
  assert(run("drift")==EFI_SUCCESS);contains("stable_active_graph=0");
  assert(run("duplicate-edge")==EFI_SUCCESS);contains("stable_active_graph=0");
  assert(run("parent-error")==EFI_SUCCESS);contains("ENDPOINT_PCI handle=0x300 status=0x800000000000000e");
  const char *failures[]={"locate-error","null-handles","too-many-handles","duplicate-handle","null-handle",
    "protocol-error","null-protocols","too-many-protocols","duplicate-protocol","null-protocol",
    "open-info-error","null-opens","too-many-opens","zero-open-count","map-error","bad-map-stride",
    "no-memory-map","read-protected","overlapping-map","unreadable-path","bad-path","path-error",
    "binding-error","unreadable-binding","image-error","unreadable-image-interface","unreadable-image",
    "oversize-image","bad-pe","bad-stop","allocation-first","allocation-map","open-error","write-error",
    "close-error","free-error","owned","no-locate-service"};
  for(unsigned i=0;i<sizeof(failures)/sizeof(*failures);i++){
    EFI_STATUS status=run(failures[i]);if(!EFI_ERROR(status)){fprintf(stderr,"unexpected success: %s\n",failures[i]);abort();}
  }
  puts("Firmware capture: 43 synthetic graph/metadata/error cases, pools balanced, source preserved, mutations=0: PASS");
  return 0;
}
