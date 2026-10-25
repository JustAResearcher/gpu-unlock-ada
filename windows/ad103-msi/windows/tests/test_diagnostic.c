/* SPDX-License-Identifier: GPL-2.0-only */
/* Full diagnostic entry point with fake EFI protocols and a synthetic ROM.
 * No firmware binary, real disk, GPU, driver, or UEFI variable is accessed. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "app_io.h"
#include "ownership.h"
#include "capture.h"
#include "compat/compat.h"
#include "gfw_eval.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DriverBinding.h>
#include <Guid/FileInfo.h>
#include <IndustryStandard/Acpi.h>
#include "../../tests/vbios_fixture.h"

static EFI_GUID pci_guid=EFI_PCI_IO_PROTOCOL_GUID, gop_guid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,
  path_guid=EFI_DEVICE_PATH_PROTOCOL_GUID, loaded_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID,
  fs_guid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, bind_guid=EFI_DRIVER_BINDING_PROTOCOL_GUID,
  text_out_guid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
static EFI_HANDLE app_handle=(VOID*)1, disk_handle=(VOID*)2, gpu_handle=(VOID*)3,
  display_handle=(VOID*)4, driver_handle=(VOID*)5, image_handle=(VOID*)6, child_handle=(VOID*)7;
static EFI_BOOT_SERVICES bs;
static EFI_SYSTEM_TABLE st;
static EFI_RUNTIME_SERVICES rt;
static EFI_PCI_IO_PROTOCOL pci;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs;
static EFI_LOADED_IMAGE_PROTOCOL loaded, driver_image;
static EFI_DRIVER_BINDING_PROTOCOL binding;
static EFI_GRAPHICS_OUTPUT_PROTOCOL gop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
static EFI_GRAPHICS_OUTPUT_PROTOCOL child_gop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE child_mode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION child_info;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL con_out;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL con_in;
static UINT32 config[64];
static UINT8 prom[GU_PROM_VIEW_BYTES];
static struct {USB_DEVICE_PATH usb;EFI_DEVICE_PATH_PROTOCOL end;} disk_path;
static EFI_DEVICE_PATH_PROTOCOL display_path;
static EFI_DEVICE_PATH_PROTOCOL child_path[129];
typedef struct { EFI_FILE_PROTOCOL p; unsigned kind; } MOCK_FILE;
static MOCK_FILE files[4];
static char report[131072], console[131072];
static size_t report_size, console_size;
static unsigned gpu_reads, pci_reads, mutation_calls, file_creates, file_closes, file_writes;
static unsigned pool_allocs, pool_frees;
static unsigned child_protocol_calls, child_info_calls;
static BOOLEAN final_flush, complete_flush;
static const char *scenario;
static BOOLEAN is(const char *s){return !strcmp(scenario,s);}
/* Capture's real metadata implementation and its production file writer have
 * dedicated harnesses. This boundary mock tests entry-point ordering/errors. */
static unsigned capture_calls;
EFI_STATUS GuCaptureFirmware(EFI_SYSTEM_TABLE *table){
  assert(table==&st && Gu.handle==gpu_handle && !Gu.armed && !Gu.owned);
  capture_calls++;return is("capture-error")?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static BOOLEAN child_case(VOID){return !strncmp(scenario,"child-",6) || is("quiet-child-no-gop");}
static BOOLEAN child_without_gop(VOID){
  return is("child-no-gop") || is("quiet-child-no-gop") || is("child-protocol-error") ||
    is("child-protocol-overflow") || is("child-open-info-error") || is("child-open-info-overflow");
}
static BOOLEAN eq(const EFI_GUID *a,const EFI_GUID *b){return !memcmp(a,b,sizeof(*a));}
static BOOLEAN wide_eq(const CHAR16 *a,const char *b){while(*a && *b)if(*a++!=(UINT8)*b++)return FALSE;return !*a && !*b;}

static EFI_STATUS EFIAPI alloc(EFI_MEMORY_TYPE type,UINTN n,VOID **out) {
  (void)type;*out=calloc(1,n);if(*out)pool_allocs++;return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS EFIAPI free_pool(VOID *p){assert(p);pool_frees++;free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,CHAR16 *s){
  (void)self;while(*s){assert(console_size+2<sizeof(console));console[console_size++]=(char)*s++;}
  console[console_size]=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI reset_input(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self,BOOLEAN extended){
  (void)self;(void)extended;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self,EFI_INPUT_KEY *k){
  (void)self;k->ScanCode=0;k->UnicodeChar='\r';return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI stall(UINTN us){(void)us;return EFI_SUCCESS;}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL tpl){assert(tpl==TPL_NOTIFY);return TPL_APPLICATION;}
static VOID EFIAPI restore_tpl(EFI_TPL tpl){assert(tpl==TPL_APPLICATION);}
static EFI_STATUS EFIAPI read_config(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
  UINT32 offset,UINTN count,VOID *out){
  (void)self;assert(width==EfiPciIoWidthUint32 && count==1 && offset<=252);
  pci_reads++;memcpy(out,(UINT8*)config+offset,4);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI mem_read(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
  UINT8 bar,UINT64 offset,UINTN count,VOID *out){
  (void)self;assert(width==EfiPciIoWidthUint32 && bar==0);gpu_reads++;
  if(is("read-error"))return EFI_DEVICE_ERROR;
  if(offset>=GU_PROM_BAR_OFFSET && offset+count*4<=GU_PROM_BAR_OFFSET+sizeof(prom)){
    memcpy(out,prom+(offset-GU_PROM_BAR_OFFSET),count*4);return EFI_SUCCESS;
  }
  assert(count==1);UINT32 value=0;
  switch(offset){
  case 0:value=0x193000a1;break;
  case 0x1183a4:value=16384;break;
  case GU_GFW_PLM_OFFSET:value=1;break;
  case GU_GFW_BOOT_OFFSET:value=0xff;break;
  case GU_GFW_CPUCTL_OFFSET:value=0x10;break;
  case 0x1fa828:value=is("warm-gpu")?0x10:0;break;
  case GU_FEAT_PLM_OFFSET:value=0xffffff8f;break;
  case GU_FEAT_SS0_OFFSET:value=0;break;
  case GU_FEAT_SS1_OFFSET:value=7;break;
  default:assert(!"Unexpected diagnostic MMIO read");
  }
  memcpy(out,&value,4);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI write_config(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
  UINT32 offset,UINTN count,VOID *data){
  (void)self;(void)width;(void)offset;(void)count;(void)data;mutation_calls++;return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI mem_write(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
  UINT8 bar,UINT64 offset,UINTN count,VOID *data){
  (void)self;(void)width;(void)bar;(void)offset;(void)count;(void)data;mutation_calls++;return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI attributes(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,
  UINT64 attrs,UINT64 *result){
  (void)self;(void)op;(void)attrs;(void)result;mutation_calls++;return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI allocate_buffer(EFI_PCI_IO_PROTOCOL *self,EFI_ALLOCATE_TYPE type,
  EFI_MEMORY_TYPE mem,UINTN pages,VOID **host,UINT64 attrs){
  (void)self;(void)type;(void)mem;(void)pages;(void)host;(void)attrs;mutation_calls++;return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI disconnect(EFI_HANDLE c,EFI_HANDLE d,EFI_HANDLE child){
  (void)c;(void)d;(void)child;mutation_calls++;return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI connect(EFI_HANDLE c,EFI_HANDLE *d,EFI_DEVICE_PATH_PROTOCOL *p,BOOLEAN recursive){
  (void)c;(void)d;(void)p;(void)recursive;mutation_calls++;return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI stop(EFI_DRIVER_BINDING_PROTOCOL *self,EFI_HANDLE c,UINTN n,EFI_HANDLE *children){
  (void)self;(void)c;(void)n;(void)children;mutation_calls++;return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI set_variable(CHAR16 *name,EFI_GUID *guid,UINT32 attrs,UINTN size,VOID *data){
  (void)name;(void)guid;(void)attrs;(void)size;(void)data;mutation_calls++;return EFI_ACCESS_DENIED;
}
static VOID EFIAPI reset_system(EFI_RESET_TYPE type,EFI_STATUS status,UINTN size,VOID *data){
  (void)type;(void)status;(void)size;(void)data;mutation_calls++;assert(!"ResetSystem was called");
}
static EFI_STATUS EFIAPI load_image(BOOLEAN policy,EFI_HANDLE parent,EFI_DEVICE_PATH_PROTOCOL *path,
  VOID *source,UINTN size,EFI_HANDLE *image){
  (void)policy;(void)parent;(void)path;(void)source;(void)size;(void)image;mutation_calls++;return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI start_image(EFI_HANDLE image,UINTN *size,CHAR16 **data){
  (void)image;(void)size;(void)data;mutation_calls++;return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI location(EFI_PCI_IO_PROTOCOL *self,UINTN *s,UINTN *b,UINTN *d,UINTN *f){
  (void)self;*s=0;*b=1;*d=0;*f=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI bar_attributes(EFI_PCI_IO_PROTOCOL *self,UINT8 bar,UINT64 *supported,VOID **resources){
  (void)self;(void)supported;assert(bar==0);
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *r=NULL;
  assert(!EFI_ERROR(alloc(EfiLoaderData,sizeof(*r)+2,(VOID**)&r)));
  r->Desc=ACPI_ADDRESS_SPACE_DESCRIPTOR;r->Len=sizeof(*r)-3;r->ResType=0;
  r->AddrSpaceGranularity=32;r->AddrRangeMin=0x90000000;r->AddrLen=is("bad-bar")?4096:0x1000000;
  r->AddrRangeMax=r->AddrRangeMin+r->AddrLen-1;
  ((UINT8*)(r+1))[0]=ACPI_END_TAG_DESCRIPTOR;*resources=r;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handles(EFI_LOCATE_SEARCH_TYPE search,EFI_GUID *guid,VOID *key,
  UINTN *count,EFI_HANDLE **out){
  (void)key;assert(search==ByProtocol);
  if(eq(guid,&pci_guid))*count=is("duplicate-target")?2:1;
  else {assert(eq(guid,&gop_guid));*count=is("multiple-displays")?2:1;}
  assert(!EFI_ERROR(alloc(EfiLoaderData,*count*sizeof(**out),(VOID**)out)));
  for(UINTN i=0;i<*count;i++)(*out)[i]=eq(guid,&pci_guid)?gpu_handle:display_handle;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI protocol(EFI_HANDLE h,EFI_GUID *guid,VOID **out){
  *out=NULL;
  if(h==app_handle && eq(guid,&loaded_guid))*out=&loaded;
  if(h==disk_handle && eq(guid,&path_guid))*out=&disk_path;
  if(h==disk_handle && eq(guid,&fs_guid))*out=&fs;
  if(h==gpu_handle && eq(guid,&pci_guid))*out=&pci;
  if(h==display_handle && eq(guid,&gop_guid))*out=&gop;
  if(h==display_handle && eq(guid,&path_guid))*out=&display_path;
  if(h==child_handle && eq(guid,&gop_guid) && !child_without_gop())*out=&child_gop;
  if(h==child_handle && eq(guid,&path_guid) && !is("child-no-path"))*out=child_path;
  if(h==driver_handle && eq(guid,&bind_guid))*out=&binding;
  if(h==image_handle && eq(guid,&loaded_guid))*out=&driver_image;
  return *out?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI locate_path(EFI_GUID *guid,EFI_DEVICE_PATH_PROTOCOL **path,EFI_HANDLE *device){
  assert(eq(guid,&pci_guid));*device=gpu_handle;
  if(*path==child_path && is("child-wrong-parent"))*device=(VOID*)8;
  if(*path==child_path && is("child-parent-error")){*device=NULL;return EFI_DEVICE_ERROR;}
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI protocols(EFI_HANDLE h,EFI_GUID ***out,UINTN *count){
  *out=NULL;*count=0;
  if(h==gpu_handle){
    *count=child_case()?2:1;
    assert(!EFI_ERROR(alloc(EfiLoaderData,*count*sizeof(**out),(VOID**)out)));
    (*out)[0]=&pci_guid;if(*count==2)(*out)[1]=&path_guid;return EFI_SUCCESS;
  }
  assert(h==display_handle || h==child_handle);child_protocol_calls++;
  if(h==child_handle && is("child-protocol-error"))return EFI_DEVICE_ERROR;
  *count=h==child_handle && is("child-protocol-overflow")?33:2;
  assert(!EFI_ERROR(alloc(EfiLoaderData,*count*sizeof(**out),(VOID**)out)));
  for(UINTN i=0;i<*count;i++)(*out)[i]=i==0?&path_guid:h==display_handle?&gop_guid:&text_out_guid;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI owners(EFI_HANDLE h,EFI_GUID *guid,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *count){
  *out=NULL;*count=0;
  if(h!=gpu_handle){
    assert(h==display_handle || h==child_handle);child_info_calls++;
    if(h==child_handle && is("child-open-info-error"))return EFI_DEVICE_ERROR;
    *count=h==child_handle && is("child-open-info-overflow")?33:1;
    assert(!EFI_ERROR(alloc(EfiLoaderData,*count*sizeof(**out),(VOID**)out)));
    for(UINTN i=0;i<*count;i++){
      (*out)[i].AgentHandle=driver_handle;(*out)[i].ControllerHandle=h;
      (*out)[i].Attributes=EFI_OPEN_PROTOCOL_GET_PROTOCOL;(*out)[i].OpenCount=1;
    }
    return EFI_SUCCESS;
  }
  assert(eq(guid,&pci_guid) || (child_case() && eq(guid,&path_guid)));
  *count=eq(guid,&pci_guid)?1:2;
  assert(!EFI_ERROR(alloc(EfiLoaderData,*count*sizeof(**out),(VOID**)out)));
  if(eq(guid,&pci_guid)){
    (*out)->AgentHandle=driver_handle;(*out)->ControllerHandle=gpu_handle;
    (*out)->Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;(*out)->OpenCount=is("bad-owner")?2:1;
  }else{
    for(UINTN i=0;i<2;i++){
      (*out)[i].AgentHandle=driver_handle;
      (*out)[i].ControllerHandle=i==0 || is("child-duplicate-record")?display_handle:child_handle;
      (*out)[i].Attributes=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER;
      (*out)[i].OpenCount=i==1 && is("child-record-policy")?2:1;
    }
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI file_open(EFI_FILE_PROTOCOL *self,EFI_FILE_PROTOCOL **out,CHAR16 *name,UINT64 flags,UINT64 attrs){
  MOCK_FILE *f=(MOCK_FILE*)self;*out=NULL;
  if(f->kind==0){
    assert(wide_eq(name,"\\GPU-UNLOCK-ADA") && !(flags&EFI_FILE_MODE_CREATE));
    if(is("missing-log-directory"))return EFI_NOT_FOUND;
    *out=&files[1].p;return EFI_SUCCESS;
  }
  if(f->kind==1){
    assert(wide_eq(name,"RUN00"));
    if(!(flags&EFI_FILE_MODE_CREATE))return EFI_NOT_FOUND;
    assert(attrs==EFI_FILE_DIRECTORY);file_creates++;*out=&files[2].p;return EFI_SUCCESS;
  }
  assert(f->kind==2 && wide_eq(name,"REPORT.TXT"));
  if(!(flags&EFI_FILE_MODE_CREATE))return EFI_NOT_FOUND;
  assert(attrs==0);file_creates++;*out=&files[3].p;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI file_info(EFI_FILE_PROTOCOL *self,EFI_GUID *guid,UINTN *size,VOID *buffer){
  (void)guid;MOCK_FILE *f=(MOCK_FILE*)self;assert(*size>=sizeof(EFI_FILE_INFO));
  memset(buffer,0,sizeof(EFI_FILE_INFO));EFI_FILE_INFO *i=buffer;
  i->Size=sizeof(*i);i->Attribute=f->kind==3?0:EFI_FILE_DIRECTORY;
  i->FileSize=f->kind==3?report_size:0;*size=sizeof(*i);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI file_write(EFI_FILE_PROTOCOL *self,UINTN *size,VOID *data){
  assert(((MOCK_FILE*)self)->kind==3);file_writes++;
  if(is("write-error"))return EFI_DEVICE_ERROR;
  if(is("short-write")){*size=0;return EFI_SUCCESS;}
  assert(report_size+*size+1<sizeof(report));
  memcpy(report+report_size,data,*size);report_size+=*size;report[report_size]=0;
  final_flush=strstr(data,"FINAL mode=diagnostic")!=NULL;
  complete_flush=strstr(data,"DIAGNOSTIC_FINISHED")!=NULL;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI flush(EFI_FILE_PROTOCOL *self){
  assert(((MOCK_FILE*)self)->kind==3);
  return is("flush-error") || (is("final-flush-error")&&final_flush) ||
    (is("complete-flush-error")&&complete_flush)?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_file(EFI_FILE_PROTOCOL *self){
  file_closes++;return is("close-error") && ((MOCK_FILE*)self)->kind==3?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,EFI_FILE_PROTOCOL **out){
  (void)self;*out=&files[0].p;return EFI_SUCCESS;
}
static VOID setup(VOID){
  bs.AllocatePool=alloc;bs.FreePool=free_pool;bs.HandleProtocol=protocol;bs.LocateHandleBuffer=handles;
  bs.LocateDevicePath=locate_path;bs.ProtocolsPerHandle=protocols;bs.OpenProtocolInformation=owners;
  bs.RaiseTPL=raise_tpl;bs.RestoreTPL=restore_tpl;bs.Stall=stall;
  bs.DisconnectController=disconnect;bs.ConnectController=connect;bs.LoadImage=load_image;bs.StartImage=start_image;
  rt.SetVariable=set_variable;rt.ResetSystem=reset_system;
  st.BootServices=&bs;st.RuntimeServices=&rt;st.ConOut=&con_out;st.ConIn=&con_in;
  con_out.OutputString=output;con_in.Reset=reset_input;con_in.ReadKeyStroke=key;
  pci.Pci.Read=read_config;pci.Pci.Write=write_config;pci.Mem.Read=mem_read;pci.Mem.Write=mem_write;
  pci.GetLocation=location;pci.GetBarAttributes=bar_attributes;pci.Attributes=attributes;pci.AllocateBuffer=allocate_buffer;
  config[0]=is("no-target")?0x123410de:GU_BOARD_ID;
  config[1]=0x00100002U|(is("bad-config")?4:0);config[2]=0x03000000;config[4]=0x90000000;
  config[11]=0xe1301462;config[0x34/4]=0x40;config[0x40/4]=0x00035001;
  config[0x50/4]=0x00020010;config[0x54/4]=1U<<28;
  vbios_fixture(prom,6);if(is("bad-prom"))prom[0]=0;
  pci.RomImage=is("no-rom")?NULL:prom;pci.RomSize=4096;
  loaded.DeviceHandle=disk_handle;
  disk_path.usb.Header.Type=is("non-usb")?HARDWARE_DEVICE_PATH:MESSAGING_DEVICE_PATH;
  disk_path.usb.Header.SubType=MSG_USB_DP;disk_path.usb.Header.Length[0]=sizeof(USB_DEVICE_PATH);
  disk_path.end.Type=END_DEVICE_PATH_TYPE;disk_path.end.SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;disk_path.end.Length[0]=4;
  display_path=disk_path.end;
  info.HorizontalResolution=1024;info.VerticalResolution=768;info.PixelsPerScanLine=1024;
  info.PixelFormat=PixelBlueGreenRedReserved8BitPerColor;
  mode.Info=&info;mode.SizeOfInfo=sizeof(info);mode.FrameBufferBase=0x10000000;mode.FrameBufferSize=1024*768*4;gop.Mode=&mode;
  child_gop=gop;child_mode=mode;child_info=info;
  child_gop.Mode=&child_mode;child_mode.Info=&child_info;
  child_path[0]=display_path;
  if(is("child-no-mode"))child_gop.Mode=NULL;
  if(is("child-no-info"))child_mode.Info=NULL;
  if(is("child-short-info")){child_mode.Info=(VOID*)1;child_mode.SizeOfInfo=0;}
  if(is("child-zero-framebuffer"))child_mode.FrameBufferSize=0;
  if(is("child-bad-geometry"))child_info.PixelsPerScanLine=100;
  if(is("child-path-length"))child_path[0].Length[0]=3;
  if(is("child-path-end"))child_path[0].SubType=END_INSTANCE_DEVICE_PATH_SUBTYPE;
  if(is("child-path-limit"))for(UINTN i=0;i<129;i++){
    child_path[i].Type=HARDWARE_DEVICE_PATH;child_path[i].SubType=1;child_path[i].Length[0]=4;
  }
  binding.DriverBindingHandle=driver_handle;binding.ImageHandle=image_handle;binding.Stop=stop;
  driver_image.ImageBase=prom;driver_image.ImageSize=4096;driver_image.ImageCodeType=EfiBootServicesCode;
  fs.OpenVolume=open_volume;
  for(unsigned n=0;n<4;n++){
    files[n].kind=n;files[n].p.Open=file_open;files[n].p.Close=close_file;
    files[n].p.GetInfo=file_info;files[n].p.Write=file_write;files[n].p.Flush=flush;
  }
}
int main(int argc,char **argv){
  assert(argc==3);scenario=argv[1];setup();
  /* These entry points are refused even when called directly, before any
   * runtime state or valid firmware protocols could authorize a mutation. */
  assert(GuLoadAssets()==EFI_ACCESS_DENIED);
  assert(GuIoConsumeSession()==EFI_ACCESS_DENIED);
  assert(GuClaimController()==EFI_ACCESS_DENIED);
  if(is("quiet-child-no-gop")){
    GU_RELEASE_OWNER owner;GuInit(&bs,&pci);Gu.handle=gpu_handle;Gu.image=app_handle;
    assert(GuFindReleaseOwner(&st,&owner,FALSE)==EFI_UNSUPPORTED);
    assert(!child_protocol_calls && !child_info_calls && !file_writes && !mutation_calls);
    assert(pool_allocs==pool_frees);
    FILE *out=fopen(argv[2],"wb");assert(out);assert(!fclose(out));
    puts("quiet child observer: mutations=0, extra metadata queries=0");return 0;
  }
  EFI_STATUS status=EfiMain(app_handle,&st);
  assert(!mutation_calls && !Gu.mmio_writes && !Gu.flrs && !Gu.dma_count && !Gu.allocations);
  assert(!Gu.started && !Gu.owned && !Gu.armed && !Gu.retained);
  assert(pool_allocs==pool_frees);
  if(is("success")){
    assert(status==EFI_SUCCESS && pci_reads>1 && gpu_reads>1000);
    assert(capture_calls==1);
    assert(strstr(report,"DIAGNOSTIC_FINISHED eligible=0 gpu_writes=0"));
    assert(strstr(report,"FINAL mode=diagnostic status=0 stage=complete gpu_writes=0\n"));
  }else assert(EFI_ERROR(status));
  if(child_case()){
    assert(capture_calls==1);
    assert(child_protocol_calls==2);
    assert(strstr(report,"display child: record=1 handle=0x4 agent=0x5 status=0\n"));
    assert(!strstr(report,"DIAGNOSTIC_COMPLETE"));
  }
  if(is("non-usb"))assert(!file_creates && !file_writes && !pci_reads && !gpu_reads);
  if(is("missing-log-directory"))assert(!file_creates && !pci_reads && !gpu_reads && file_closes==1);
  if(is("no-target") || is("duplicate-target") || is("bad-config") || is("bad-bar"))assert(!gpu_reads);
  if(is("write-error") || is("short-write") || is("flush-error"))assert(!pci_reads && !gpu_reads);
  if(is("close-error")){
    assert(file_closes==4);
    assert(strstr(console,"USB report close failed"));
  }
  FILE *out=fopen(argv[2],"wb");assert(out);assert(fwrite(report,1,report_size,out)==report_size);assert(!fclose(out));
  printf("diagnostic mock %s: status=%llx, PCI_reads=%u MMIO_reads=%u mutations=%u USB_writes=%u\n",
    scenario,(unsigned long long)status,pci_reads,gpu_reads,mutation_calls,file_writes);
  return 0;
}
