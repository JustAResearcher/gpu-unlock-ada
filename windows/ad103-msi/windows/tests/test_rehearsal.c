/* SPDX-License-Identifier: GPL-2.0-only */
/* Synthetic EFI driver release/reconnect fixture. No captured firmware bytes. */
#define _GNU_SOURCE
#include "rehearsal.h"
#include "app_io.h"
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <IndustryStandard/Acpi10.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define H(n) ((EFI_HANDLE)(UINTN)(n))
#define GPU H(0x100)
#define OLD_GOP H(0x200)
#define OLD_EDID H(0x300)
#define CONSOLE H(0x400)
#define NVIDIA H(0x500)
#define GRAPHICS H(0x600)
#define TEXT H(0x700)
#define KEYBOARD H(0x800)
#define KEY_SPLITTER H(0x900)
#define APP H(0xa00)
#define NEW_GOP H(0xb00)
#define NEW_EDID H(0xc00)
#define FOREIGN H(0xd00)
#define ROOT_BRIDGE H(0x1000)
#define PCI_BUS H(0x1100)
#define USB_HOST H(0x1200)
#define USB_BUS H(0x1300)
#define USB_CHILD H(0x1400)
#define USB_CONSUMER H(0x1500)
#define USB_OTHER H(0x1600)
#define USB_IDLE H(0x1700)
#define USB_OLD(i) H(0x2000+(i)*0x100)
#define USB_NEW(i) H(0x3000+(i)*0x100)
#define USB_NEXT(i) H(0x4000+(i)*0x100)
#define IMAGE_BYTES 4096
#define OPT 0x98
#define SECTIONS (OPT+240)

GU_PLATFORM Gu;
BOOLEAN GuScreenDetached;
static struct {
  EFI_BOOT_SERVICES bs;
  EFI_SYSTEM_TABLE table;
  EFI_PCI_IO_PROTOCOL pci;
  UINT32 config[64];
  UINT64 attributes;
  UINT8 *images[3];
  EFI_DRIVER_BINDING_PROTOCOL bindings[3];
  EFI_LOADED_IMAGE_PROTOCOL loaded[3],app_loaded;
  UINT8 paths[6][36];
  UINT8 usb_host_path[64],usb_child_path[96],usb_other_path[128];
  UINT8 usb_host_interface[256],usb_alternate_interface[256],usb_child_interface[256];
  UINT8 replacement_paths[7][64],replacement_io[3][7][256];
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL output,physical_output;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL input,physical_input;
  EFI_GRAPHICS_OUTPUT_PROTOCOL gop,virtual_gop,alternate_gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode,virtual_mode,alternate_mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info,alternate_info;
} f;
static EFI_GUID pci_guid=EFI_PCI_IO_PROTOCOL_GUID,path_guid=EFI_DEVICE_PATH_PROTOCOL_GUID,
  gop_guid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,text_guid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID,
  input_guid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID,binding_guid=EFI_DRIVER_BINDING_PROTOCOL_GUID,
  image_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID edid_guid={0x1c0c34f6,0xd380,0x41fa,{0xa0,0x49,0x8a,0xd0,0x6c,0x1a,0x66,0xaa}};
static EFI_GUID unknown_guid={0x12345678,0x1234,0x5678,{0x90,0xab,0xcd,0xef,0x01,0x23,0x45,0x67}};
/* Synthetic tags model protocols produced by console consumers. Their
 * identities and timing are fixture data, not captured vendor firmware. */
static EFI_GUID console_device_tag_guid={0x12345679,0x1234,0x5678,{0x90,0xab,0xcd,0xef,0x01,0x23,0x45,0x67}};
static EFI_GUID console_vendor_tag_guid={0x1234567a,0x1234,0x5678,{0x90,0xab,0xcd,0xef,0x01,0x23,0x45,0x67}};
static EFI_GUID root_guid={0x2f707ebb,0x4a1a,0x11d4,{0x9a,0x38,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
static EFI_GUID usb_host_guid={0x3e745226,0x9818,0x45b6,{0xa2,0xac,0xd7,0xcd,0x0e,0x8b,0xa2,0xbc}};
static EFI_GUID usb_io_guid={0x2b2f68d6,0x0cd2,0x44cf,{0x8e,0x8b,0xbb,0xa2,0x0b,0x1b,0x5b,0x75}};
static EFI_GUID block_io_guid={0x964e5b21,0x6459,0x11d2,{0x8e,0x39,0,0xa0,0xc9,0x69,0x72,0x3b}};
static const char *scenario;
static EFI_TPL current_tpl=TPL_APPLICATION;
static unsigned raises,restores;
static BOOLEAN usb_retired,usb_log_mutated;
static unsigned replacement_generation,replacement_notes;
static unsigned usb_missing_path_queries;
static struct {VOID *pointer;UINTN bytes;} fixture_pool[128];

static unsigned stage,detaches,connects,supported_calls,allocations,frees,alloc_attempts,snapshots,forbidden,logs;
static unsigned protected_code;
static BOOLEAN log_ok=TRUE,alternate_interface;
static BOOLEAN is(const char *name){return !strcmp(scenario,name);}
static BOOLEAN usb_case(void){return !strncmp(scenario,"usb-",4);}
static BOOLEAN replacement_case(void){return !strncmp(scenario,"usb-replace-",12);}
static BOOLEAN replacement_refusal(void){return !strncmp(scenario,"usb-replace-refuse-",19);}
static BOOLEAN replacement_stage4(void){return !strncmp(scenario,"usb-replace-stage4",18);}
static BOOLEAN replacement_final(void){return !strncmp(scenario,"usb-replace-final",17);}
static BOOLEAN replacement_success(void){return replacement_case()&&!replacement_refusal()&&!is("usb-replace-stage4-refuse")&&!is("usb-replace-log-keyboard");}
static BOOLEAN replacement_bad(const char *suffix){
  char name[96];snprintf(name,sizeof(name),"usb-replace-refuse-%s",suffix);return is(name);
}
static EFI_HANDLE replacement_handle(unsigned i){
  if(!replacement_generation)return USB_OLD(i);
  if(is("usb-replace-preflight-partial")&&i>=3)return USB_OLD(i);
  if(replacement_bad("reuse-child")&&i==0)return USB_OLD(1);
  if(replacement_bad("reuse-other")&&i==0)return USB_OTHER;
  if(replacement_bad("reuse-idle")&&i==0)return USB_IDLE;
  if(replacement_bad("reuse-permuted")&&i<2)return USB_OLD(i+1);
  return replacement_generation==1?USB_NEW(i):USB_NEXT(i);
}
static unsigned replacement_count(void){
  if(!replacement_generation)return 6;
  if(replacement_bad("extra"))return 7;
  if(is("usb-replace-preflight-retirement"))return 5;
  return 6;
}
static BOOLEAN replacement_old_alias(void){return replacement_generation&&replacement_bad("old-alias");}
static int replacement_index(EFI_HANDLE handle){
  if(!replacement_case())return -1;
  for(unsigned i=0;i<replacement_count();i++)if(handle==replacement_handle(i))return (int)i;
  if(replacement_old_alias()&&handle==USB_OLD(0))return 0;
  return -1;
}
static unsigned replacement_io_generation(unsigned i){
  return replacement_handle(i)==USB_OLD(i)?0:replacement_generation;
}
static BOOLEAN replacement_other_path(void){
  return replacement_generation&&(replacement_bad("descendant")||replacement_bad("alias"));
}
static void replacement_change(void){
  replacement_generation++;
  if(replacement_bad("image-device"))f.app_loaded.DeviceHandle=replacement_handle(0);
  if(replacement_bad("image-parent"))f.app_loaded.ParentHandle=replacement_handle(0);
  if(replacement_bad("host-path"))f.usb_host_path[17]=0x15;
}
static BOOLEAN usb_success(void){return replacement_success()||is("usb-retire")||is("usb-retire-absent")||is("usb-retire-final")||is("usb-final-extra-missing")||is("usb-multi-unrelated");}
static EFI_HANDLE usb_child(void){return is("usb-keyboard-child")?KEYBOARD:USB_CHILD;}
static BOOLEAN usb_ancestor_case(void){return !strncmp(scenario,"usb-ancestor-",13);}
static BOOLEAN usb_other_has_path(void){return (usb_ancestor_case()&&(!is("usb-ancestor-late")||usb_retired))||is("usb-multi-unrelated")||is("usb-multi-descendant");}
static BOOLEAN usb_child_present(void){return !(usb_retired&&is("usb-retire-absent"));}
static EFI_TPL EFIAPI fixture_raise(EFI_TPL level){
  assert(current_tpl==TPL_APPLICATION&&level==TPL_NOTIFY);raises++;current_tpl=level;return TPL_APPLICATION;
}
static VOID EFIAPI fixture_restore(EFI_TPL level){
  assert(current_tpl==TPL_NOTIFY&&level==TPL_APPLICATION);restores++;current_tpl=level;
}
static void fixture_log_event(const char *format){
  assert(current_tpl==TPL_APPLICATION);
  if(strstr(format,"REHEARSAL_GRAPH_USB_LEAF_REPLACED"))replacement_notes++;
  if(replacement_case()&&!replacement_generation&&
     ((!replacement_stage4()&&!replacement_final()&&stage==0&&strstr(format,"REHEARSAL_USB_LEAF_CLASSIFIED"))||
      (replacement_stage4()&&stage==3&&strstr(format,"REHEARSAL_INTENT"))))replacement_change();
  if(replacement_case()&&replacement_generation&&is("usb-replace-log-keyboard")&&
     strstr(format,"REHEARSAL_GRAPH_"))usb_log_mutated=TRUE;
  if(usb_case()&&!replacement_case()&&!is("usb-retire-final")&&!is("usb-final-extra-missing")&&strstr(format,"REHEARSAL_INTENT")&&stage==3)usb_retired=TRUE;
  if(usb_retired&&stage==3&&strstr(format,"REHEARSAL_GRAPH_")&&
     (is("usb-log-extra-missing")||is("usb-log-added-edge")))usb_log_mutated=TRUE;
  if(usb_retired&&stage==3&&strstr(format,"SYNTHETIC_POSTSTATE_LOG")&&is("usb-poststate-extra-missing"))usb_log_mutated=TRUE;
  if(usb_retired&&is("usb-host-path-drift"))f.usb_host_path[17]=0x15;
  if(usb_retired&&is("usb-child-path-drift"))f.usb_child_path[22]=1;
}
static BOOLEAN deferred_case(void){return !strncmp(scenario,"deferred-",9);}
static void active_mode(void){
  f.mode.MaxMode=1;f.mode.Mode=0;f.mode.FrameBufferBase=0x6800000000ULL;
  f.mode.FrameBufferSize=2560*1440*4;
}
static void deferred_mode(void){
  f.mode.MaxMode=1;f.mode.Mode=2;f.mode.FrameBufferBase=0;f.mode.FrameBufferSize=0;
}
static BOOLEAN equal_guid(const EFI_GUID *a,const EFI_GUID *b){return !memcmp(a,b,sizeof(*a));}
static EFI_HANDLE display(void){
  if(stage<4||is("success-reused-handles")||is("deferred-success-reused-handles"))return OLD_GOP;
  if(is("restore-cross-role"))return OLD_EDID;
  if(is("restore-unrelated-reuse"))return KEYBOARD;
  if(is("restore-driver-reuse"))return NVIDIA;
  return NEW_GOP;
}
static EFI_HANDLE edid(void){
  if(stage<4||is("success-reused-handles")||is("deferred-success-reused-handles"))return OLD_EDID;
  if(is("restore-cross-role"))return OLD_GOP;
  return NEW_EDID;
}
static BOOLEAN has_children(void){return stage!=3;}
static BOOLEAN has_text(void){return stage<2||stage>=5;}
static BOOLEAN text_owned(void){return stage==0||stage==6;}
static BOOLEAN gfx_owned(void){return stage<2||stage>=5;}
static BOOLEAN nv_owned(void){return stage!=3;}
static VOID *pool(UINTN bytes){
  alloc_attempts++;
  if(is("allocation-first")&&alloc_attempts==1)return NULL;
  VOID *value=calloc(1,bytes?bytes:1);assert(value);allocations++;
  for(UINTN i=0;i<128;i++)if(!fixture_pool[i].pointer){fixture_pool[i].pointer=value;fixture_pool[i].bytes=bytes?bytes:1;return value;}
  assert(!"fixture pool tracking exhausted");return NULL;
}
VOID *GuAlloc(size_t bytes){return pool(bytes);}
static EFI_STATUS EFIAPI allocate_pool(EFI_MEMORY_TYPE type,UINTN bytes,VOID **out){(void)type;*out=pool(bytes);return *out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;}
static EFI_STATUS EFIAPI free_pool(VOID *value){
  assert(value);BOOLEAN found=FALSE;for(UINTN i=0;i<128;i++)if(fixture_pool[i].pointer==value){fixture_pool[i].pointer=NULL;found=TRUE;break;}
  assert(found);frees++;free(value);return is("free-error")?EFI_DEVICE_ERROR:is("free-warning")?EFI_WARN_UNKNOWN_GLYPH:EFI_SUCCESS;}
VOID GuFree(VOID *value){if(value&&EFI_ERROR(free_pool(value)))Gu.fault=TRUE;}
BOOLEAN GuLogReady(VOID){return log_ok;}
VOID GuLog(const char *format,...){
  fixture_log_event(format);logs++;va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);va_end(ap);
  if(is("log-before-first")&&strstr(format,"INTENT"))log_ok=FALSE;
}
VOID GuLogText(const char *text){GuLog("%s",text);}
VOID GuConsole(const char *text){(void)text;forbidden++;}
BOOLEAN GuTargetConfig(const UINT32 *c,UINTN seg,UINTN bus,UINTN dev,UINTN fun){
  return c[0]==0x270510de&&c[11]==0xe1301462&&!seg&&bus==1&&!dev&&!fun;
}
EFI_STATUS GuConfigRead(EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 offset,VOID *value){
  return f.pci.Pci.Read(&f.pci,width,offset,1,value);
}
UINT32 GuRead32(UINT32 offset){(void)offset;forbidden++;return 0;}
VOID GuWrite32(UINT32 offset,UINT32 value){(void)offset;(void)value;forbidden++;}
static EFI_STATUS EFIAPI config_read(EFI_PCI_IO_PROTOCOL *io,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 offset,UINTN count,VOID *out){
  assert(io==&f.pci);assert(width==EfiPciIoWidthUint32||width==EfiPciIoWidthUint16);
  UINTN size=width==EfiPciIoWidthUint32?4:2;assert(offset+size*count<=sizeof(f.config));
  if(is("config-read-error"))return EFI_DEVICE_ERROR;
  if(is("config-read-warning"))return EFI_WARN_UNKNOWN_GLYPH;
  memcpy(out,(UINT8*)f.config+offset,count*size);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI config_write(EFI_PCI_IO_PROTOCOL *io,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 offset,UINTN count,VOID *out){
  (void)io;(void)width;(void)offset;(void)count;(void)out;forbidden++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI location(EFI_PCI_IO_PROTOCOL *io,UINTN *segment,UINTN *bus,UINTN *device,UINTN *function){
  assert(io==&f.pci);*segment=0;*bus=is("wrong-bdf")?2:1;*device=*function=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI attributes(EFI_PCI_IO_PROTOCOL *io,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 attrs,UINT64 *out){
  assert(io==&f.pci);(void)attrs;
  if(op!=EfiPciIoAttributeOperationGet){forbidden++;return EFI_DEVICE_ERROR;}
  if(is("attributes-error"))return EFI_DEVICE_ERROR;
  if(is("attributes-warning"))return EFI_WARN_UNKNOWN_GLYPH;assert(out);*out=f.attributes;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI bar(EFI_PCI_IO_PROTOCOL *io,UINT8 index,UINT64 *supports,VOID **resource){
  assert(io==&f.pci&&index==0);if(supports)*supports=0;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=pool(sizeof(*d)+2);if(!d)return EFI_OUT_OF_RESOURCES;d->Desc=0x8a;d->Len=43;d->ResType=0;
  d->AddrSpaceGranularity=32;d->AddrRangeMin=0x83000000;d->AddrRangeMax=0x83ffffff;d->AddrLen=0x1000000;
  ((UINT8*)(d+1))[0]=0x79;*resource=d;
  if(is("bar-drift"))d->AddrRangeMin+=0x1000000;return is("bar-warning")?EFI_WARN_UNKNOWN_GLYPH:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI text_reset(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *a,BOOLEAN b){(void)a;(void)b;forbidden++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI text_output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *a,CHAR16 *b){(void)a;(void)b;forbidden++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI text_mode(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *a,UINTN b){(void)a;(void)b;forbidden++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI input_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *a,BOOLEAN b){(void)a;(void)b;forbidden++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI input_read(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *a,EFI_INPUT_KEY *b){(void)a;(void)b;forbidden++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI gop_query(EFI_GRAPHICS_OUTPUT_PROTOCOL *a,UINT32 b,UINTN *c,EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **d){(void)a;(void)b;(void)c;(void)d;forbidden++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI gop_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *a,UINT32 b){(void)a;(void)b;forbidden++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI gop_blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *a,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *b,EFI_GRAPHICS_OUTPUT_BLT_OPERATION c,UINTN d,UINTN e,UINTN h,UINTN i,UINTN j,UINTN k,UINTN l){(void)a;(void)b;(void)c;(void)d;(void)e;(void)h;(void)i;(void)j;(void)k;(void)l;forbidden++;return EFI_DEVICE_ERROR;}
static EFI_STATUS EFIAPI open_protocol(EFI_HANDLE a,EFI_GUID *b,VOID **c,EFI_HANDLE d,EFI_HANDLE e,UINT32 flags){
  (void)a;(void)b;(void)c;(void)d;(void)e;(void)flags;forbidden++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI close_protocol(EFI_HANDLE a,EFI_GUID *b,EFI_HANDLE c,EFI_HANDLE d){
  (void)a;(void)b;(void)c;(void)d;forbidden++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *count,EFI_HANDLE **out){
  (void)key;EFI_HANDLE values[40];UINTN n=0;*count=0;*out=NULL;
  if(type==AllHandles){
    assert(current_tpl==TPL_NOTIFY);snapshots++;if(is("locate-error"))return EFI_DEVICE_ERROR;
    if(is("locate-warning"))return EFI_WARN_UNKNOWN_GLYPH;
    values[n++]=GPU;if(has_children()){values[n++]=display();values[n++]=edid();}
    values[n++]=CONSOLE;values[n++]=NVIDIA;values[n++]=GRAPHICS;values[n++]=TEXT;
    values[n++]=KEYBOARD;values[n++]=KEY_SPLITTER;values[n++]=APP;
    values[n++]=ROOT_BRIDGE;values[n++]=PCI_BUS;
    if(usb_case()){
      values[n++]=USB_HOST;values[n++]=USB_BUS;
      if(replacement_case()){
        unsigned count=replacement_count();
        for(unsigned i=0;i<count;i++)values[n++]=replacement_handle(is("usb-replace-preflight-reversed")?count-1-i:i);
        if(replacement_old_alias())values[n++]=USB_OLD(0);
        if(!(replacement_generation&&replacement_bad("reuse-idle")))values[n++]=USB_IDLE;
      }else if(usb_child_present()&&usb_child()!=KEYBOARD)values[n++]=USB_CHILD;
      values[n++]=USB_CONSUMER;
      if(!(replacement_case()&&replacement_generation&&replacement_bad("reuse-other")))values[n++]=USB_OTHER;
    }
    if(is("duplicate-handle"))values[n-1]=values[0];
    if(is("null-handle"))values[n-1]=NULL;
  }else if(type==ByProtocol&&equal_guid(guid,&gop_guid)){
    if(has_children())values[n++]=display();
    values[n++]=CONSOLE; /* framebuffer-free virtual GOP, including while detached */
    if(is("second-physical-gop")||(stage==4&&is("restore-display-foreign")))values[n++]=FOREIGN;
    if(stage==4&&is("restore-display-duplicate"))values[n++]=display();
  }else if(type==ByProtocol&&equal_guid(guid,&path_guid)){
    values[n++]=GPU;if(has_children()){values[n++]=display();values[n++]=edid();}
  }else if(type==ByProtocol&&equal_guid(guid,&binding_guid)){
    values[n++]=TEXT;values[n++]=GRAPHICS;values[n++]=NVIDIA;
  }else{fprintf(stderr,"unexpected locate type=%u guid=%08x\n",type,guid?guid->Data1:0);abort();}
  if(!n)return EFI_NOT_FOUND;
  if(is("null-handles")&&type==AllHandles){*count=n;return EFI_SUCCESS;}
  *out=pool(n*sizeof(**out));if(!*out)return EFI_OUT_OF_RESOURCES;memcpy(*out,values,n*sizeof(**out));*count=n;
  if(is("too-many-handles")&&type==AllHandles)*count=4097;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI protocols(EFI_HANDLE handle,EFI_GUID ***out,UINTN *count){
  EFI_GUID *list[10];UINTN n=0;*out=NULL;*count=0;
  if(handle==GPU){list[n++]=&pci_guid;list[n++]=&path_guid;}
  else if(has_children()&&handle==display()){
    list[n++]=&gop_guid;
    if(has_text()&&!(stage==6&&is("deferred-stage6-missing-text")))list[n++]=&text_guid;
    list[n++]=&path_guid;
    /* Both tags are captured at baseline and absent immediately after NVIDIA
     * Start. One returns with GraphicsConsole, the other with the text consumer.
     * This cannot be derived by subtracting only SimpleTextOut from baseline. */
    if((stage==0||stage>=6)&&!(stage==6&&is("deferred-stage6-missing-console-tag")))
      list[n++]=&console_device_tag_guid;
    if((stage<2||stage>=5)&&!(stage==6&&is("deferred-stage6-missing-vendor-tag")))
      list[n++]=&console_vendor_tag_guid;
    if((stage>=4&&is("deferred-stage6-protocol-extra"))||
      (stage==4&&is("deferred-success-transient-tag")))list[n++]=&unknown_guid;
  }else if(has_children()&&handle==edid()){list[n++]=&edid_guid;list[n++]=&path_guid;}
  else if(handle==CONSOLE){list[n++]=&text_guid;list[n++]=&input_guid;list[n++]=&gop_guid;}
  else if(handle==NVIDIA||handle==GRAPHICS||handle==TEXT){list[n++]=&binding_guid;list[n++]=&image_guid;}
  else if(handle==KEYBOARD){list[n++]=&input_guid;list[n++]=&path_guid;}
  else if(handle==ROOT_BRIDGE){list[n++]=&root_guid;}
  else if(usb_case()&&handle==USB_HOST){list[n++]=&usb_host_guid;list[n++]=&path_guid;}
  else if(replacement_case()&&replacement_index(handle)>=0){
    list[n++]=&path_guid;list[n++]=&usb_io_guid;
    if(replacement_generation&&replacement_index(handle)==0&&replacement_bad("protocol"))list[n++]=&block_io_guid;
  }
  else if(usb_case()&&handle==USB_CHILD&&usb_child_present()){
    list[n++]=&path_guid;list[n++]=&usb_io_guid;
    if(is("usb-storage-child"))list[n++]=&block_io_guid;
    if(is("usb-child-protocol-extra"))list[n++]=&unknown_guid;
    if(usb_retired&&is("usb-child-protocol-added"))list[n++]=&unknown_guid;
  }
  else if(replacement_case()&&handle==USB_OTHER&&replacement_bad("wrong-host")){
    list[n++]=&usb_host_guid;list[n++]=&path_guid;
  }
  else if(replacement_case()&&handle==USB_OTHER&&replacement_other_path()){
    list[n++]=&path_guid;list[n++]=&block_io_guid;
  }
  else if(usb_case()&&handle==USB_OTHER&&usb_other_has_path()){
    list[n++]=&path_guid;list[n++]=is("usb-ancestor-keyboard")?&input_guid:&block_io_guid;
  }
  else if(usb_case()&&handle==APP){list[n++]=&image_guid;}
  else if(handle==KEY_SPLITTER||handle==APP||handle==PCI_BUS||
          (usb_case()&&(handle==USB_BUS||handle==USB_CONSUMER||handle==USB_OTHER||handle==USB_IDLE))){list[n++]=&unknown_guid;}
  else return EFI_UNSUPPORTED;
  if(handle==GPU&&is("unknown-protocol"))list[n++]=&unknown_guid;
  if(handle==GPU&&is("protocol-error"))return EFI_DEVICE_ERROR;
  if(handle==GPU&&is("protocol-warning"))return EFI_WARN_UNKNOWN_GLYPH;
  if(handle==GPU&&is("null-protocols")){*count=n;return EFI_SUCCESS;}
  *out=pool(n*sizeof(**out));if(!*out)return EFI_OUT_OF_RESOURCES;memcpy(*out,list,n*sizeof(**out));*count=n;
  if(handle==GPU&&is("duplicate-protocol"))(*out)[1]=(*out)[0];
  if(handle==GPU&&is("null-protocol"))(*out)[1]=NULL;
  if(handle==GPU&&is("too-many-protocols"))*count=65;return EFI_SUCCESS;
}
static EFI_OPEN_PROTOCOL_INFORMATION_ENTRY ent(EFI_HANDLE agent,EFI_HANDLE controller,UINT32 attrs,UINT32 count){
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY e={agent,controller,attrs,count};return e;
}
static EFI_STATUS EFIAPI information(EFI_HANDLE handle,EFI_GUID *guid,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *count){
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY entries[16];UINTN n=0;*out=NULL;*count=0;
  if(handle==GPU&&equal_guid(guid,&pci_guid)&&nv_owned())entries[n++]=ent(NVIDIA,GPU,0x10,1);
  if(handle==GPU&&equal_guid(guid,&path_guid)&&has_children()){
    entries[n++]=ent(NVIDIA,display(),8,1);entries[n++]=ent(NVIDIA,edid(),8,1);
  }
  if(handle==display()&&has_children()&&equal_guid(guid,&gop_guid)&&gfx_owned())entries[n++]=ent(GRAPHICS,display(),0x10,1);
  if(handle==display()&&has_children()&&equal_guid(guid,&text_guid)&&text_owned()){
    entries[n++]=ent(TEXT,display(),0x10,1);entries[n++]=ent(TEXT,CONSOLE,8,1);
  }
  if(usb_case()&&handle==USB_HOST&&equal_guid(guid,&usb_host_guid)){
    if(!(usb_retired&&is("usb-host-owner-lost"))&&!(replacement_generation&&replacement_bad("host-owner")))entries[n++]=ent(USB_BUS,USB_HOST,0x10,1);
    if(replacement_case()){
      for(unsigned i=0;i<replacement_count();i++){
        if(replacement_generation&&i==0&&replacement_bad("wrong-host"))continue;
        EFI_HANDLE agent=replacement_generation&&i==0&&replacement_bad("agent")?USB_CONSUMER:USB_BUS;
        UINT32 attrs=replacement_generation&&i==0&&replacement_bad("attributes")?0x10:8;
        UINT32 count=replacement_generation&&i==0&&replacement_bad("count")?2:1;
        entries[n++]=ent(agent,replacement_handle(i),attrs,count);
      }
    }else if(!usb_retired)entries[n++]=ent(USB_BUS,usb_child(),8,is("usb-count")?2:1);
    if(usb_retired&&is("usb-agent"))entries[n++]=ent(USB_CONSUMER,usb_child(),8,1);
    if(usb_retired&&is("usb-added-child"))entries[n++]=ent(USB_BUS,USB_OTHER,8,1);
  }
  if(replacement_case()&&replacement_generation&&replacement_index(handle)==0&&equal_guid(guid,&usb_io_guid)&&replacement_bad("active"))
    entries[n++]=ent(USB_CONSUMER,handle,0x10,1);
  if(replacement_case()&&handle==USB_OTHER&&equal_guid(guid,&usb_host_guid)&&replacement_bad("wrong-host")){
    entries[n++]=ent(USB_BUS,USB_OTHER,0x10,1);
    if(replacement_generation)entries[n++]=ent(USB_BUS,replacement_handle(0),8,1);
  }
  if(usb_case()&&handle==USB_CHILD&&equal_guid(guid,&usb_io_guid)){
    if(is("usb-active-child")||is("usb-storage-child")||(usb_retired&&is("usb-child-active-now")))
      entries[n++]=ent(USB_CONSUMER,USB_CHILD,0x10,1);
  }
  if(usb_case()&&handle==USB_OTHER&&usb_other_has_path()&&is("usb-ancestor-keyboard")&&equal_guid(guid,&input_guid)){
    entries[n++]=ent(KEY_SPLITTER,USB_OTHER,0x10,1);entries[n++]=ent(KEY_SPLITTER,CONSOLE,8,1);
  }
  if(usb_case()&&handle==USB_OTHER&&equal_guid(guid,&unknown_guid)&&
     ((usb_retired&&is("usb-added-edge"))||(usb_log_mutated&&is("usb-log-added-edge"))))
    entries[n++]=ent(USB_CONSUMER,USB_OTHER,0x10,1);
  if(handle==KEYBOARD&&equal_guid(guid,&input_guid)){
    if(!(replacement_generation&&(is("usb-replace-stage4-refuse")||is("usb-replace-final-refuse"))))entries[n++]=ent(KEY_SPLITTER,KEYBOARD,0x10,1);
    if(!(stage&&is("keyboard-child-removed"))&&!(usb_retired&&(is("usb-extra-missing")||is("usb-final-extra-missing")))&&!(usb_log_mutated&&(is("usb-log-extra-missing")||is("usb-poststate-extra-missing")||is("usb-replace-log-keyboard"))))entries[n++]=ent(KEY_SPLITTER,CONSOLE,8,(stage&&is("keyboard-count-drift"))||(stage==4&&is("deferred-keyboard-edge"))?2:1);
  }
  if(handle==ROOT_BRIDGE&&equal_guid(guid,&root_guid)){
    if(!is("root-parent-missing"))entries[n++]=ent(PCI_BUS,GPU,8,1);
    if(!is("root-driver-missing"))entries[n++]=ent(PCI_BUS,ROOT_BRIDGE,0x10,1);
    if(is("root-parent-duplicate"))entries[n++]=ent(FOREIGN,GPU,8,1);
    if(is("unknown-incoming"))entries[n++]=ent(PCI_BUS,display(),8,1);
  }
  if(handle==GPU&&equal_guid(guid,&pci_guid)){
    if(is("unknown-consumer"))entries[n++]=ent(FOREIGN,GPU,0x10,1);
    if((stage&&is("new-consumer"))||(stage==4&&is("deferred-new-consumer")))entries[n++]=ent(FOREIGN,GPU,0x10,1);
    if(is("zero-open-count")&&n)entries[0].OpenCount=0;
    if(is("duplicate-edge")&&n)entries[n++]=entries[0];
    if(is("null-agent")&&n)entries[0].AgentHandle=NULL;
    if(is("unknown-attributes")&&n)entries[0].Attributes=0x80;
    if(is("information-error"))return EFI_DEVICE_ERROR;
    if(is("information-warning"))return EFI_WARN_UNKNOWN_GLYPH;
    if(is("null-opens")){*count=1;return EFI_SUCCESS;}
  }
  if(n){*out=pool(n*sizeof(**out));if(!*out)return EFI_OUT_OF_RESOURCES;memcpy(*out,entries,n*sizeof(**out));*count=n;}
  if(handle==GPU&&equal_guid(guid,&pci_guid)&&is("too-many-opens"))*count=129;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI interface(EFI_HANDLE handle,EFI_GUID *guid,VOID **out){
  *out=NULL;
  if(usb_case()&&handle==APP&&equal_guid(guid,&image_guid)){*out=&f.app_loaded;return EFI_SUCCESS;}
  if(handle==GPU&&equal_guid(guid,&pci_guid)&&is("interface-warning"))return EFI_WARN_UNKNOWN_GLYPH;
  if(handle==GPU&&equal_guid(guid,&pci_guid)){*out=&f.pci;return EFI_SUCCESS;}
  if(replacement_case()&&handle==USB_OTHER&&equal_guid(guid,&usb_host_guid)&&replacement_bad("wrong-host")){*out=f.usb_alternate_interface;return EFI_SUCCESS;}
  if(usb_case()&&handle==USB_HOST&&equal_guid(guid,&usb_host_guid)){
    if(replacement_generation&&replacement_bad("host-interface")){*out=f.usb_alternate_interface;return EFI_SUCCESS;}
    *out=usb_retired&&is("usb-host-interface-drift")?f.usb_alternate_interface:f.usb_host_interface;
    if(is("usb-host-interface-unreadable"))*out=(VOID*)0x1000;
    return EFI_SUCCESS;
  }
  if(replacement_case()&&replacement_index(handle)>=0&&equal_guid(guid,&usb_io_guid)){
    unsigned i=(unsigned)replacement_index(handle);
    *out=replacement_generation&&i==0&&replacement_bad("interface-unreadable")?(VOID*)0x1000:f.replacement_io[replacement_old_alias()&&handle==USB_OLD(0)?0:replacement_io_generation(i)][i];return EFI_SUCCESS;
  }
  if(usb_case()&&handle==USB_CHILD&&equal_guid(guid,&usb_io_guid)){
    if(!usb_child_present()){usb_missing_path_queries++;return EFI_UNSUPPORTED;}
    *out=usb_retired&&is("usb-child-interface-drift")?f.usb_alternate_interface:f.usb_child_interface;return EFI_SUCCESS;
  }
  if(equal_guid(guid,&path_guid)){
    if(replacement_case()&&replacement_index(handle)>=0){
      unsigned i=(unsigned)replacement_index(handle);
      if(replacement_generation&&i==0&&replacement_bad("path-unreadable"))*out=(VOID*)0x1000;
      else if(replacement_generation&&i==1&&replacement_bad("duplicate"))*out=f.replacement_paths[0];
      else if(replacement_generation&&i==0&&replacement_bad("path"))*out=f.replacement_paths[6];
      else *out=f.replacement_paths[i];
      return EFI_SUCCESS;
    }
    if(replacement_case()&&handle==USB_OTHER&&replacement_other_path()){*out=f.usb_other_path;return EFI_SUCCESS;}
    if(usb_case()&&handle==USB_HOST){*out=f.usb_host_path;return EFI_SUCCESS;}
    if(usb_case()&&handle==USB_OTHER&&usb_other_has_path()){*out=f.usb_other_path;return EFI_SUCCESS;}
    if(usb_case()&&handle==USB_CHILD){
      if(!usb_child_present()){usb_missing_path_queries++;return EFI_UNSUPPORTED;}
      *out=is("usb-child-path-unreadable")?(VOID*)0x1000:f.usb_child_path;return EFI_SUCCESS;
    }
    if(handle==GPU)*out=f.paths[0];
    else if(has_children()&&handle==display())*out=f.paths[1];
    else if(has_children()&&handle==edid())*out=f.paths[2];
    else if(handle==KEYBOARD)*out=f.paths[5];
    else return EFI_UNSUPPORTED;
    if(handle==edid()&&is("unreadable-path"))*out=(VOID*)0x1000;
    return EFI_SUCCESS;
  }
  if(equal_guid(guid,&gop_guid)&&handle==CONSOLE){*out=&f.virtual_gop;return EFI_SUCCESS;}
  if(equal_guid(guid,&gop_guid)&&((has_children()&&handle==display())||handle==FOREIGN)){
    if(stage==4&&is("restore-display-interface-error"))return EFI_DEVICE_ERROR;
    *out=stage==4&&is("restore-display-unreadable-interface")?(VOID*)0x1000:
      alternate_interface?&f.alternate_gop:&f.gop;return EFI_SUCCESS;
  }
  if(equal_guid(guid,&text_guid)){
    if(handle==CONSOLE)*out=&f.output;
    else if(has_children()&&handle==display()&&has_text())*out=&f.physical_output;
    else return EFI_UNSUPPORTED;return EFI_SUCCESS;
  }
  if(equal_guid(guid,&input_guid)){
    if(handle==CONSOLE)*out=&f.input;else if(handle==KEYBOARD)*out=&f.physical_input;else return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
  }
  unsigned index=handle==TEXT?0:handle==GRAPHICS?1:handle==NVIDIA?2:3;
  if(index<3&&equal_guid(guid,&binding_guid)){
    if(index==0&&is("binding-error"))return EFI_DEVICE_ERROR;
    *out=index==0&&is("unreadable-binding")?(VOID*)0x1000:&f.bindings[index];return EFI_SUCCESS;
  }
  if(index<3&&equal_guid(guid,&image_guid)){
    if(index==0&&is("image-error"))return EFI_DEVICE_ERROR;
    *out=index==0&&is("unreadable-image-interface")?(VOID*)0x1000:&f.loaded[index];return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI locate_path(EFI_GUID *guid,EFI_DEVICE_PATH_PROTOCOL **path,EFI_HANDLE *handle){
  assert(equal_guid(guid,&pci_guid));assert(*path);*handle=is("wrong-parent")?FOREIGN:GPU;
  return is("parent-error")?EFI_NOT_FOUND:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI memory_map(UINTN *bytes,EFI_MEMORY_DESCRIPTOR *map,UINTN *key,UINTN *stride,UINT32 *version){
  if(is("map-error"))return EFI_DEVICE_ERROR;
  if(is("map-warning"))return EFI_WARN_UNKNOWN_GLYPH;
  EFI_MEMORY_DESCRIPTOR d;memset(&d,0,sizeof(d));d.Type=is("map-conventional")?EfiConventionalMemory:EfiBootServicesData;
  d.PhysicalStart=(UINTN)&f&~(UINT64)4095;UINT64 end=((UINTN)&f+sizeof(f)+4095)&~(UINT64)4095;
  d.NumberOfPages=(end-d.PhysicalStart)/4096;d.Attribute=is("read-protected")?EFI_MEMORY_RP:0;
  assert(*bytes>=6*sizeof(d));memcpy(map,&d,sizeof(d));*bytes=sizeof(d);*stride=sizeof(d);*version=1;*key=1;
  if(is("overlapping-map")){memcpy((UINT8*)map+*bytes,&d,sizeof(d));*bytes+=sizeof(d);}
  for(unsigned i=0;i<3;i++){
    d.Type=is("code-in-data-map")&&i==1?EfiBootServicesData:EfiBootServicesCode;
    d.Attribute=is("read-protected")?EFI_MEMORY_RP:0;
    char xp_case[32];snprintf(xp_case,sizeof(xp_case),"code-xp-%u",i+1);
    if(is(xp_case)||(protected_code==i+1)||
       (is("late-code-xp")&&stage>=1&&i==0)||
       (is("late-nv-code-xp")&&stage>=3&&i==2))d.Attribute|=EFI_MEMORY_XP;
    d.PhysicalStart=(UINTN)f.images[i];d.NumberOfPages=1;
    memcpy((UINT8*)map+*bytes,&d,sizeof(d));*bytes+=sizeof(d);
  }
  if(is("interior-overlap")||is("code-interior-overlap")){
    d.PhysicalStart=(UINTN)f.images[0]+(is("code-interior-overlap")?0x280:0x800);
    memcpy((UINT8*)map+*bytes,&d,sizeof(d));*bytes+=sizeof(d);
  }
  if(usb_case()){
    /* Model firmware-owned protocol arrays and GUID storage for safe classifier
     * reads. This does not broaden any malformed-map fixture. */
    EFI_MEMORY_DESCRIPTOR all[160];UINTN n=*bytes/sizeof(*map);memcpy(all,map,*bytes);
    const VOID *guids[]={&pci_guid,&path_guid,&gop_guid,&text_guid,&input_guid,&binding_guid,
      &image_guid,&edid_guid,&unknown_guid,&console_device_tag_guid,&console_vendor_tag_guid,&root_guid,
      &usb_host_guid,&usb_io_guid,&block_io_guid};
    for(UINTN i=0;i<128+sizeof(guids)/sizeof(*guids);i++){
      const VOID *p=i<128?fixture_pool[i].pointer:guids[i-128];
      UINTN size=i<128?fixture_pool[i].bytes:sizeof(EFI_GUID);if(!p)continue;
      EFI_MEMORY_DESCRIPTOR extra={0};extra.Type=EfiBootServicesData;
      extra.PhysicalStart=(UINTN)p&~(UINT64)4095;
      UINT64 end=((UINTN)p+size+4095)&~(UINT64)4095;
      extra.NumberOfPages=(end-extra.PhysicalStart)/4096;assert(n<160);all[n++]=extra;
    }
    for(UINTN i=1;i<n;i++)for(UINTN j=i;j&&all[j].PhysicalStart<all[j-1].PhysicalStart;j--){
      EFI_MEMORY_DESCRIPTOR x=all[j];all[j]=all[j-1];all[j-1]=x;
    }
    UINTN used=0;
    for(UINTN i=0;i<n;i++){
      EFI_MEMORY_DESCRIPTOR *x=&all[i];
      if(used){
        EFI_MEMORY_DESCRIPTOR *last=&all[used-1];UINT64 end=last->PhysicalStart+last->NumberOfPages*4096;
        if(last->Type==x->Type&&last->Attribute==x->Attribute&&x->PhysicalStart<=end){
          UINT64 next=x->PhysicalStart+x->NumberOfPages*4096;
          if(next>end)last->NumberOfPages=(next-last->PhysicalStart)/4096;continue;
        }
      }
      all[used++]=*x;
    }
    *bytes=used*sizeof(*map);memcpy(map,all,*bytes);
  }
  if(is("bad-map-stride"))*stride=1;if(is("bad-map-version"))*version=2;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI disconnect(EFI_HANDLE controller,EFI_HANDLE driver,EFI_HANDLE child){
  assert(child==NULL);assert(detaches<3&&connects==0);assert(GuRehearsalMutated());
  EFI_HANDLE expected[]={TEXT,GRAPHICS,NVIDIA};assert(driver==expected[detaches]);
  assert(controller==(detaches==2?GPU:OLD_GOP));detaches++;
  char name[64];snprintf(name,sizeof(name),"detach-error-%u",detaches);
  if(is(name))return EFI_DEVICE_ERROR;
  snprintf(name,sizeof(name),"detach-warning-%u",detaches);if(is(name))return EFI_WARN_UNKNOWN_GLYPH;
  snprintf(name,sizeof(name),"detach-unchanged-%u",detaches);
  if(!is(name))stage=detaches;
  snprintf(name,sizeof(name),"detach-partial-%u",detaches);
  if(is(name))return EFI_DEVICE_ERROR;
  if(stage==3){f.config[1]&=~3u;f.attributes&=~(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY);}
  if(is("config-drift"))f.config[4]^=0x1000000;
  if(is("bme-set"))f.config[1]|=4;
  if(is("console-callback-drift"))f.output.OutputString=NULL;
  if(is("key-callback-drift"))f.input.ReadKeyStroke=NULL;
  if(is("image-drift"))f.images[1][0x250]^=1;
  if(is("gop-path-drift"))f.paths[1][4]^=1;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI connect_forbidden(EFI_HANDLE controller,EFI_HANDLE *drivers,EFI_DEVICE_PATH_PROTOCOL *remaining,BOOLEAN recursive){
  (void)controller;(void)drivers;(void)remaining;(void)recursive;forbidden++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI supported(EFI_DRIVER_BINDING_PROTOCOL *binding,EFI_HANDLE controller,EFI_DEVICE_PATH_PROTOCOL *remaining){
  assert(detaches==3&&connects<3&&!remaining);assert(binding==&f.bindings[2-connects]);
  assert(controller==(connects==0?GPU:display()));supported_calls++;
  char name[64];snprintf(name,sizeof(name),"supported-error-%u",connects+1);if(is(name))return EFI_UNSUPPORTED;
  snprintf(name,sizeof(name),"supported-warning-%u",connects+1);if(is(name))return EFI_WARN_UNKNOWN_GLYPH;
  snprintf(name,sizeof(name),"supported-mutation-%u",connects+1);if(is(name))f.config[4]^=0x1000000;
  snprintf(name,sizeof(name),"supported-identity-%u",connects+1);if(is(name))f.images[2-connects][0x250]^=1;
  snprintf(name,sizeof(name),"supported-code-xp-%u",connects+1);if(is(name))protected_code=3-connects;
  if(is("supported-decode-1")){
    f.config[1]|=3;
    f.attributes|=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
  }
  if(connects==1){
    if(is("deferred-supported-active"))active_mode();
    if(is("active-supported-deferred"))deferred_mode();
    if(is("deferred-supported-mode-pointer")){f.alternate_mode=f.mode;f.gop.Mode=&f.alternate_mode;}
    if(is("deferred-supported-info-pointer")){f.alternate_info=f.info;f.mode.Info=&f.alternate_info;}
    if(is("deferred-supported-interface-pointer")){f.alternate_gop=f.gop;alternate_interface=TRUE;}
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI binding_stop(EFI_DRIVER_BINDING_PROTOCOL *binding,EFI_HANDLE controller,UINTN count,EFI_HANDLE *children){
  (void)binding;(void)controller;(void)count;(void)children;forbidden++;return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI binding_start(EFI_DRIVER_BINDING_PROTOCOL *binding,EFI_HANDLE controller,EFI_DEVICE_PATH_PROTOCOL *remaining){
  assert(detaches==3&&connects<3&&!remaining);assert(binding==&f.bindings[2-connects]);assert(supported_calls==connects+1);
  assert(controller==(connects==0?GPU:display()));connects++;
  char name[64];snprintf(name,sizeof(name),"connect-error-%u",connects);
  if(is(name))return EFI_DEVICE_ERROR;
  snprintf(name,sizeof(name),"connect-warning-%u",connects);if(is(name))return EFI_WARN_UNKNOWN_GLYPH;
  snprintf(name,sizeof(name),"connect-unchanged-%u",connects);
  if(!is(name))stage=3+connects;
  snprintf(name,sizeof(name),"connect-partial-%u",connects);
  if(is(name))return EFI_DEVICE_ERROR;
  if(stage>=4){f.config[1]|=3;f.attributes|=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;}
  if(is("restore-config-drift"))f.config[4]^=0x1000000;
  if(is("restore-attributes-drift"))f.attributes^=EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY;
  if(is("restore-path-drift"))f.paths[2][4]^=1;
  /* Deferred display fixtures model initialization inside the existing
   * GraphicsConsole driver Start. The application must never call GOP methods. */
  if(deferred_case()&&stage==4){
    deferred_mode();
    if(is("deferred-mode-valid"))f.mode.Mode=0;
    if(is("deferred-mode-other"))f.mode.Mode=3;
    if(is("deferred-max-mode")){f.mode.MaxMode=2;f.mode.Mode=3;}
    if(is("deferred-nonzero-base"))f.mode.FrameBufferBase=0x6800000000ULL;
    if(is("deferred-nonzero-size"))f.mode.FrameBufferSize=2560*1440*4;
    if(is("deferred-info-drift"))f.info.PixelInformation.RedMask=0xff;
    if(is("deferred-width-drift"))f.info.HorizontalResolution--;
    if(is("deferred-pixel-drift"))f.info.PixelFormat=PixelRedGreenBlueReserved8BitPerColor;
    if(is("deferred-stride-drift"))f.info.PixelsPerScanLine++;
    if(is("deferred-info-size"))f.mode.SizeOfInfo--;
    if(is("deferred-unreadable-info"))f.mode.Info=(VOID*)0x1000;
    if(is("deferred-callback-drift"))f.gop.QueryMode=(EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(VOID*)(f.images[2]+0x220);
    if(is("deferred-driver-code"))f.images[1][0x250]^=1;
    if(is("deferred-path-drift"))f.paths[2][4]^=1;
    if(is("deferred-bme")){f.config[1]|=4;f.attributes|=EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;}
    if(is("deferred-config-drift"))f.config[4]^=0x1000000;
    if(is("deferred-console-drift"))f.input.ReadKeyStroke=NULL;
  }
  if(deferred_case()&&stage==5){
    if(!is("deferred-still-uninitialized"))active_mode();
    if(is("deferred-stage5-mode-drift"))f.mode.Mode=1;
    if(is("deferred-stage5-info-drift"))f.info.HorizontalResolution--;
  }
  if(stage==6&&is("deferred-stage6-dormant"))deferred_mode();
  /* The NVIDIA Start reports success but the freshly installed display is
   * invalid or differs from the cold baseline. No GraphicsConsole/Text Start
   * may follow these failures, and diagnostic logging must not dereference
   * any unreadable pointer or invoke a GOP method. */
  if(stage==4){
    if(is("restore-display-virtual-fb")){
      f.virtual_mode.FrameBufferBase=f.mode.FrameBufferBase;
      f.virtual_mode.FrameBufferSize=f.mode.FrameBufferSize;
    }
    if(is("restore-display-fb-base"))f.mode.FrameBufferBase=0;
    if(is("restore-display-fb-size"))f.mode.FrameBufferSize=0;
    if(is("restore-display-query-null"))f.gop.QueryMode=NULL;
    if(is("restore-display-set-null"))f.gop.SetMode=NULL;
    if(is("restore-display-blt-null"))f.gop.Blt=NULL;
    if(is("restore-display-unreadable-mode"))f.gop.Mode=(VOID*)0x1000;
    if(is("restore-display-unreadable-info"))f.mode.Info=(VOID*)0x1000;
    if(is("restore-display-null-info"))f.mode.Info=NULL;
    if(is("restore-display-small-info"))f.mode.SizeOfInfo=sizeof(f.info)-1;
    if(is("restore-display-zero-width"))f.info.HorizontalResolution=0;
    if(is("restore-display-zero-height"))f.info.VerticalResolution=0;
    if(is("restore-display-pixel-blt"))f.info.PixelFormat=PixelBltOnly;
    if(is("restore-display-pixel-invalid"))f.info.PixelFormat=PixelFormatMax;
    if(is("restore-display-short-stride"))f.info.PixelsPerScanLine--;
    if(is("restore-display-short-buffer"))f.mode.FrameBufferSize--;
    if(is("restore-display-mode-drift"))f.mode.Mode++;
    if(is("restore-display-max-mode-drift"))f.mode.MaxMode++;
    if(is("restore-display-fb-drift"))f.mode.FrameBufferBase+=4096;
    if(is("restore-display-info-drift"))f.info.PixelInformation.RedMask=0xff;
    if(is("restore-display-callback-drift"))f.gop.QueryMode=(EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(VOID*)(f.images[2]+0x220);
  }
  return EFI_SUCCESS;
}
static void p16(UINT8 *p,UINT16 v){p[0]=(UINT8)v;p[1]=(UINT8)(v>>8);}
static void p32(UINT8 *p,UINT32 v){p16(p,(UINT16)v);p16(p+2,(UINT16)(v>>16));}
static void p64(UINT8 *p,UINT64 v){p32(p,(UINT32)v);p32(p+4,(UINT32)(v>>32));}
static void make_pe(UINT8 *p){
  p[0]='M';p[1]='Z';p32(p+60,0x80);memcpy(p+0x80,"PE\0\0",4);p16(p+0x84,0x8664);p16(p+0x86,2);
  p16(p+0x94,240);p16(p+OPT,0x20b);p32(p+OPT+16,0x220);p64(p+OPT+24,(UINTN)p);
  p32(p+OPT+56,IMAGE_BYTES);p32(p+OPT+60,0x200);p32(p+OPT+108,6);memcpy(p+SECTIONS,".text",5);
  p32(p+SECTIONS+8,0x200);p32(p+SECTIONS+12,0x200);p32(p+SECTIONS+16,0x200);p32(p+SECTIONS+36,0x60000020);
  memcpy(p+SECTIONS+40,".data",5);p32(p+SECTIONS+48,0x200);p32(p+SECTIONS+52,0x400);p32(p+SECTIONS+56,0x200);p32(p+SECTIONS+76,0xc0000040);
  memset(p+0x200,0x5a,0x200);
  /* Fixed synthetic code jumps through data slots to host fixture callbacks.
   * Never execute captured firmware; no production callback-validation bypass. */
  UINTN offsets[]={0x220,0x230,0x240};
  for(unsigned i=0;i<3;i++){
    UINTN at=offsets[i];p[at]=0xff;p[at+1]=0x25;p32(p+at+2,(UINT32)(0x400+8*i-at-6));
  }
  p64(p+0x400,(UINTN)binding_stop);p64(p+0x408,(UINTN)binding_start);p64(p+0x410,(UINTN)supported);
}
static void setup(const char *name){
  scenario=name;memset(&Gu,0,sizeof(Gu));memset(&f,0,sizeof(f));
  f.bs.AllocatePool=allocate_pool;f.bs.FreePool=free_pool;f.bs.LocateHandleBuffer=locate;f.bs.ProtocolsPerHandle=protocols;
  f.bs.OpenProtocolInformation=information;f.bs.HandleProtocol=interface;f.bs.LocateDevicePath=locate_path;
  f.bs.GetMemoryMap=memory_map;f.bs.DisconnectController=disconnect;f.bs.ConnectController=connect_forbidden;
  f.bs.OpenProtocol=open_protocol;f.bs.CloseProtocol=close_protocol;
  f.bs.RaiseTPL=fixture_raise;f.bs.RestoreTPL=fixture_restore;
  f.table.BootServices=&f.bs;f.table.ConsoleOutHandle=f.table.ConsoleInHandle=f.table.StandardErrorHandle=CONSOLE;
  f.table.ConOut=f.table.StdErr=&f.output;f.table.ConIn=&f.input;
  f.output.Reset=text_reset;f.output.OutputString=text_output;f.output.SetMode=text_mode;
  f.physical_output=f.output;
  f.input.Reset=input_reset;f.input.ReadKeyStroke=input_read;f.input.WaitForKey=(VOID*)0xe00;
  f.physical_input=f.input;
  for(unsigned i=0;i<6;i++){
    UINT8 *p=f.paths[i];p[0]=1;p[1]=1;p[2]=6;p[4]=(UINT8)i;p[6]=127;p[7]=255;p[8]=4;
  }
  const UINT8 host_path[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,0x14,127,255,4,0};
  const UINT8 child_tail[]={3,5,6,0,0,0,127,255,4,0};
  memcpy(f.usb_host_path,host_path,sizeof(host_path));
  memcpy(f.usb_child_path,host_path,sizeof(host_path)-4);
  memcpy(f.usb_child_path+sizeof(host_path)-4,child_tail,sizeof(child_tail));
  if(is("usb-host-path-bad"))f.usb_host_path[2]=0;
  if(is("usb-child-path-bad"))f.usb_child_path[20]=0;
  if(is("usb-child-path-not-usb"))f.usb_child_path[19]=6;
  if(is("usb-child-path-prefix"))f.usb_child_path[17]=0x15;
  if(is("usb-child-path-multi-instance"))f.usb_child_path[25]=1;
  memcpy(f.usb_other_path,f.usb_child_path,24);
  memcpy(f.usb_other_path+24,child_tail,sizeof(child_tail));
  if(is("usb-ancestor-alias"))memcpy(f.usb_other_path,f.usb_child_path,28);
  if(is("usb-multi-unrelated")||is("usb-multi-descendant")){
    memcpy(f.usb_other_path,f.usb_child_path,28);f.usb_other_path[22]=1;f.usb_other_path[25]=1;
    memcpy(f.usb_other_path+28,f.usb_child_path,28);
    if(is("usb-multi-unrelated"))f.usb_other_path[50]=2;
  }
  for(unsigned i=0;i<7;i++){
    UINT8 *path=f.replacement_paths[i];memcpy(path,host_path,18);
    const UINT8 branch[]={3,5,6,0,4,0,3,5,6,0,3,0,3,5,6,0,0,0,127,255,4,0};
    memcpy(path+18,branch,sizeof(branch));path[35]=(UINT8)i;
  }
  if(replacement_case()&&(replacement_bad("descendant")||replacement_bad("alias"))){
    memcpy(f.usb_other_path,f.replacement_paths[0],40);
    if(replacement_bad("descendant")){memcpy(f.usb_other_path+36,child_tail,sizeof(child_tail));}
  }
  f.app_loaded.DeviceHandle=is("usb-loaded-image-child")?USB_CHILD:
    is("usb-ancestor-keyboard")?FOREIGN:USB_OTHER;
  EFI_HANDLE drivers[]={TEXT,GRAPHICS,NVIDIA};
  for(unsigned i=0;i<3;i++){
    f.images[i]=mmap(NULL,IMAGE_BYTES,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(f.images[i]!=MAP_FAILED);
    make_pe(f.images[i]);f.bindings[i].ImageHandle=drivers[i];f.bindings[i].DriverBindingHandle=drivers[i];f.bindings[i].Version=0x10;
    f.bindings[i].Stop=(EFI_DRIVER_BINDING_STOP)(VOID*)(f.images[i]+0x220);
    f.bindings[i].Start=(EFI_DRIVER_BINDING_START)(VOID*)(f.images[i]+0x230);
    f.bindings[i].Supported=(EFI_DRIVER_BINDING_SUPPORTED)(VOID*)(f.images[i]+0x240);
    f.loaded[i].ImageBase=f.images[i];f.loaded[i].ImageSize=IMAGE_BYTES;
    f.loaded[i].ImageCodeType=EfiBootServicesCode;f.loaded[i].ImageDataType=EfiBootServicesData;
    f.loaded[i].DeviceHandle=i==2?GPU:FOREIGN;f.loaded[i].FilePath=(VOID*)f.paths[3];
  }
  f.output.Reset=(EFI_TEXT_RESET)(VOID*)(f.images[0]+0x220);
  f.output.OutputString=(EFI_TEXT_STRING)(VOID*)(f.images[0]+0x220);
  f.output.SetMode=(EFI_TEXT_SET_MODE)(VOID*)(f.images[0]+0x220);
  f.input.Reset=(EFI_INPUT_RESET)(VOID*)(f.images[0]+0x220);
  f.input.ReadKeyStroke=(EFI_INPUT_READ_KEY)(VOID*)(f.images[0]+0x220);
  if(is("wrong-console-code"))f.output.OutputString=(EFI_TEXT_STRING)(VOID*)(f.images[0]+0x230);
  f.pci.Pci.Read=config_read;f.pci.Pci.Write=config_write;f.pci.GetLocation=location;f.pci.GetBarAttributes=bar;f.pci.Attributes=attributes;
  f.config[0]=0x270510de;f.config[1]=3;f.config[2]=0x030000a1;f.config[4]=0x83000000;f.config[11]=0xe1301462;
  f.attributes=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
  f.gop.QueryMode=gop_query;f.gop.SetMode=gop_mode;f.gop.Blt=gop_blt;
  active_mode();
  f.gop.Mode=&f.mode;f.mode.Info=&f.info;f.mode.SizeOfInfo=sizeof(f.info);f.mode.FrameBufferBase=0x6800000000ULL;
  f.mode.FrameBufferSize=2560*1440*4;f.info.HorizontalResolution=2560;f.info.VerticalResolution=1440;
  f.info.PixelsPerScanLine=2560;f.info.PixelFormat=PixelBlueGreenRedReserved8BitPerColor;
  if(is("baseline-pin-mode"))f.mode.Mode=1;
  if(is("baseline-pin-max"))f.mode.MaxMode=2;
  if(is("baseline-pin-framebuffer"))f.mode.FrameBufferBase+=4096;
  if(is("baseline-pin-bytes"))f.mode.FrameBufferSize+=4096;
  if(is("baseline-pin-info-size"))f.mode.SizeOfInfo+=4;
  if(is("baseline-pin-info"))f.info.PixelInformation.RedMask=0xff;
  f.virtual_gop=f.gop;f.virtual_gop.Mode=&f.virtual_mode;
  f.virtual_mode=f.mode;f.virtual_mode.FrameBufferBase=0;f.virtual_mode.FrameBufferSize=0;
  Gu.bs=&f.bs;Gu.pci=&f.pci;Gu.handle=GPU;Gu.image=APP;Gu.bar_base=0x83000000;Gu.bar_size=0x1000000;
  if(is("wrong-subsystem"))f.config[11]^=1;
  if(is("unreadable-image"))f.loaded[0].ImageBase=(VOID*)0x1000;
  if(is("wrong-image-size"))f.loaded[0].ImageSize--;
  if(is("bad-pe"))f.images[0][0]=0;
  if(is("wrong-code-hash"))f.images[0][0x250]^=1;
  if(is("wrong-stop"))f.bindings[0].Stop=(EFI_DRIVER_BINDING_STOP)(VOID*)(f.images[0]+0x224);
  if(is("wrong-start"))f.bindings[0].Start=(EFI_DRIVER_BINDING_START)(VOID*)(f.images[0]+0x234);
  if(is("wrong-supported"))f.bindings[0].Supported=(EFI_DRIVER_BINDING_SUPPORTED)(VOID*)(f.images[0]+0x244);
  if(is("wrong-driver-version"))f.bindings[0].Version++;
  if(is("wrong-image-source"))f.loaded[2].DeviceHandle=FOREIGN;
  if(is("wrong-driver-path"))f.paths[3][4]^=1;
  if(is("bad-path"))f.paths[2][2]=0;
  if(is("no-memory-map"))f.bs.GetMemoryMap=NULL;
  if(is("owned"))Gu.owned=TRUE;if(is("armed"))Gu.armed=TRUE;if(is("started"))Gu.started=TRUE;
  if(is("no-disconnect"))f.bs.DisconnectController=NULL;
  if(is("no-raise-tpl"))f.bs.RaiseTPL=NULL;
  if(is("no-restore-tpl"))f.bs.RestoreTPL=NULL;
  if(is("no-connect"))f.bs.ConnectController=NULL;
  if(is("no-log"))log_ok=FALSE;
}
static void cleanup(void){
  assert(allocations==frees);assert(current_tpl==TPL_APPLICATION&&raises==restores);
  assert(!usb_missing_path_queries);
  for(UINTN i=0;i<128;i++)assert(!fixture_pool[i].pointer);
  for(unsigned i=0;i<3;i++)assert(munmap(f.images[i],IMAGE_BYTES)==0);
}
int main(int argc,char **argv){
  assert(argc==2);setup(argv[1]);
  assert(EFI_ERROR(GuRehearsalRun()));assert(EFI_ERROR(GuRehearsalRestored()));
  EFI_STATUS prepared=GuRehearsalPrepare(&f.table);
  assert(forbidden==0&&detaches==0&&connects==0&&!GuRehearsalMutated());
  BOOLEAN expected_preflight=usb_case()||deferred_case()||is("active-supported-deferred")||is("success")||is("success-reused-handles")||is("late-refusal")||is("late-restored-drift")||
    is("late-code-xp")||is("late-nv-code-xp")||
    !strncmp(scenario,"detach-",7)||!strncmp(scenario,"connect-",8)||!strncmp(scenario,"supported-",10)||
    is("keyboard-child-removed")||is("keyboard-count-drift")||is("new-consumer")||
    is("config-drift")||is("bme-set")||is("console-callback-drift")||is("key-callback-drift")||
    is("image-drift")||is("gop-path-drift")||!strncmp(scenario,"restore-",8)||is("log-before-first");
  if(replacement_case())assert(replacement_generation==(unsigned)(!replacement_stage4()&&!replacement_final()));
  if(replacement_refusal())expected_preflight=FALSE;
  if(!expected_preflight){
    if(!strncmp(scenario,"baseline-pin-",13))assert(prepared==EFI_NOT_READY);
    if(!EFI_ERROR(prepared)){fprintf(stderr,"preflight unexpectedly passed: %s\n",scenario);abort();}
    cleanup();printf("%s: preflight refused; mutations=0: PASS\n",scenario);return 0;
  }
  if(EFI_ERROR(prepared)){fprintf(stderr,"prepare failed unexpectedly %s: %#llx\n",scenario,(unsigned long long)prepared);abort();}
  if(is("late-refusal"))f.images[0][0x250]^=1;
  EFI_STATUS status=GuRehearsalRun();
  assert(forbidden==0&&!Gu.mmio_writes&&!Gu.started&&!Gu.owned);
  if(usb_success()||is("success")||is("success-reused-handles")||is("late-restored-drift")||
    is("deferred-success")||is("deferred-success-reused-handles")||is("deferred-final-dormant")||
    is("deferred-success-transient-tag")){
    assert(status==EFI_SUCCESS&&stage==6&&detaches==3&&connects==3&&GuRehearsalMutated());
    if(is("late-restored-drift")){f.input.WaitForKey=(VOID*)0xdead;assert(EFI_ERROR(GuRehearsalRestored()));}
    else if(is("deferred-final-dormant")){deferred_mode();assert(EFI_ERROR(GuRehearsalRestored()));}
    else {
      if(is("usb-retire-final")||is("usb-final-extra-missing")){usb_retired=TRUE;log_ok=FALSE;}
      if(replacement_final()){replacement_change();log_ok=FALSE;}
      if(is("usb-replace-generation-two")){replacement_change();log_ok=FALSE;}
      unsigned final_logs=logs;
      assert((GuRehearsalRestored()==EFI_SUCCESS)==!(is("usb-final-extra-missing")||is("usb-replace-final-refuse")));
      if(is("usb-retire-final")||is("usb-final-extra-missing")||replacement_final()||is("usb-replace-generation-two"))assert(logs==final_logs);
    }
  }else{
    if(!EFI_ERROR(status)){fprintf(stderr,"run unexpectedly passed: %s\n",scenario);abort();}
    if(is("late-refusal")||is("log-before-first"))assert(detaches==0&&connects==0&&!GuRehearsalMutated());
    if(!strncmp(scenario,"detach-",7)){assert(connects==0);assert(detaches==(unsigned)atoi(strrchr(scenario,'-')+1));}
    if(!strncmp(scenario,"connect-",8)){assert(detaches==3);assert(connects==(unsigned)atoi(strrchr(scenario,'-')+1));}
    if(!strncmp(scenario,"supported-",10)){
      unsigned at=(unsigned)atoi(strrchr(scenario,'-')+1);
      assert(supported_calls==at&&connects==at-1&&detaches==3);
    }
    if(!strncmp(scenario,"restore-display-",16)){
      EFI_STATUS expected=(is("restore-display-interface-error")||is("restore-display-unreadable-interface")||
        is("restore-display-unreadable-mode"))?EFI_COMPROMISED_DATA:
        (is("restore-display-mode-drift")||is("restore-display-max-mode-drift")||
         is("restore-display-fb-drift")||is("restore-display-info-drift")||
         is("restore-display-callback-drift"))?EFI_NOT_READY:EFI_UNSUPPORTED;
      assert(status==expected&&stage==4&&detaches==3&&connects==1&&supported_calls==1&&GuRehearsalMutated());
      assert(EFI_ERROR(GuRehearsalRestored()));
    }
    if(deferred_case()||is("active-supported-deferred")){
      assert(detaches==3&&GuRehearsalMutated());
      if(!strncmp(scenario,"deferred-stage6-",16))assert(stage==6&&connects==3&&supported_calls==3);
      else if(is("deferred-still-uninitialized")||!strncmp(scenario,"deferred-stage5-",16))
        assert(stage==5&&connects==2&&supported_calls==2);
      else if(!strncmp(scenario,"deferred-supported-",19)||is("active-supported-deferred"))
        assert(stage==4&&connects==1&&supported_calls==2&&status==EFI_NOT_READY);
      else assert(stage==4&&connects==1&&supported_calls==1);
      assert(EFI_ERROR(GuRehearsalRestored()));
    }
    if(is("usb-replace-log-keyboard"))assert(detaches==0&&connects==0&&supported_calls==0&&stage==0&&!GuRehearsalMutated());
    else if(usb_case())assert(detaches==3&&connects==0&&supported_calls==
      (unsigned)(is("usb-log-extra-missing")||is("usb-log-added-edge"))&&stage==3);
    if(is("gop-path-drift"))assert(detaches==1&&connects==0);
    if(is("late-code-xp"))assert(detaches==1&&connects==0&&supported_calls==0);
    if(is("late-nv-code-xp"))assert(detaches==3&&connects==0&&supported_calls==0);
  }
  if(replacement_success())assert(replacement_notes==(replacement_final()?0U:
    is("usb-replace-preflight-partial")?3U:is("usb-replace-preflight-retirement")?5U:6U));
  unsigned saved_detaches=detaches,saved_connects=connects,saved_supported=supported_calls;
  assert(EFI_ERROR(GuRehearsalRun()));assert(EFI_ERROR(GuRehearsalPrepare(&f.table)));
  assert(detaches==saved_detaches&&connects==saved_connects&&supported_calls==saved_supported);
  cleanup();printf("%s: detach=%u restart=%u direct-hardware-writes=0: PASS\n",scenario,detaches,connects);
  return 0;
}
