/* SPDX-License-Identifier: GPL-2.0-only */
/* Synthetic EFI only; production handoff included to exercise its private ABI
 * views and on-disk parser without installing a driver or touching hardware. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/rehearsal_handoff.c"
#define H(n) ((EFI_HANDLE)(UINTN)(n))
GU_PLATFORM Gu;
static EFI_BOOT_SERVICES bs;
static EFI_SYSTEM_TABLE table;
static const char *scenario;
static unsigned pools,loads,starts,unloads,reads,closes,restored_calls;
static BOOLEAN mutated,log_ready=TRUE;
static struct {
  PARTITION_INFO partition[2];BLOCK_MEDIA media[2];BLOCK_IO block[2];
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs[2];EFI_FILE_PROTOCOL root[2],file[2];
  UINT8 path[2][128],disk[1024],loaded[8192],loaded_path[128];
  EFI_LOADED_IMAGE_PROTOCOL li;
} a;
static UINTN positions[2];
static BOOLEAN Is(const char *s){return !strcmp(scenario,s);}
static VOID W16(UINT8 *p,UINT16 n){p[0]=(UINT8)n;p[1]=(UINT8)(n>>8);}
static VOID W32(UINT8 *p,UINT32 n){W16(p,(UINT16)n);W16(p+2,(UINT16)(n>>16));}
static EFI_STATUS EFIAPI Alloc(EFI_MEMORY_TYPE type,UINTN n,VOID **p){
  (void)type;if(Is("allocation-failure")){*p=NULL;return EFI_OUT_OF_RESOURCES;}
  *p=calloc(1,n);if(!*p)return EFI_OUT_OF_RESOURCES;pools++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Release(VOID *p){assert(p&&pools);pools--;free(p);return EFI_SUCCESS;}
VOID *GuAlloc(size_t n){VOID *p=NULL;return Alloc(EfiLoaderData,n,&p)==EFI_SUCCESS?p:NULL;}
VOID GuFree(VOID *p){if(p)Release(p);}
VOID GuLog(const char *format,...){(void)format;}
BOOLEAN GuLogReady(VOID){return log_ready&&!Is("log-failure");}
BOOLEAN GuRehearsalMutated(VOID){return mutated;}
EFI_STATUS GuRehearsalRestored(VOID){restored_calls++;return Is("late-graph")?EFI_NOT_READY:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Map(UINTN *n,EFI_MEMORY_DESCRIPTOR *d,UINTN *key,UINTN *stride,UINT32 *version){
  if(Is("reentry")) {
    assert(GuRehearsalWindowsPrepare(&table)==EFI_ALREADY_STARTED);
    assert(GuRehearsalWindowsCancel()==EFI_ACCESS_DENIED);
    assert(GuRehearsalWindowsStart()==EFI_NOT_READY);
  }
  if(Is("map-error"))return EFI_DEVICE_ERROR;
  assert(*n>=sizeof(*d));memset(d,0,sizeof(*d));d->Type=EfiLoaderData;
  d->PhysicalStart=0;d->NumberOfPages=((UINT64)1<<48)/4096;
  if(Is("unmapped"))d->NumberOfPages=1;
  *n=sizeof(*d);*key=1;*stride=sizeof(*d);*version=1;
  if(Is("map-version"))*version=2;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handles(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **h){
  (void)key;assert(type==ByProtocol&&!memcmp(guid,&FsGuid,16));
  *n=(Is("ambiguous")||Is("duplicate-handle"))?2:1;
  *h=GuAlloc(*n*sizeof(**h));assert(*h);(*h)[0]=H(0x10);if(*n==2)(*h)[1]=Is("duplicate-handle")?H(0x10):H(0x20);
  return Is("enumeration-error")?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *g,VOID **p){
  *p=NULL;
  if(h==H(0x90)&&!memcmp(g,&ImageGuid,16)){*p=&a.li;return EFI_SUCCESS;}
  if(h!=H(0x10)&&h!=H(0x20))return EFI_UNSUPPORTED;
  UINTN i=h==H(0x10)?0:1;
  if(!memcmp(g,&PartitionGuid,16)){if(Is("missing-partition"))return EFI_UNSUPPORTED;*p=&a.partition[i];}
  else if(!memcmp(g,&BlockGuid,16))*p=&a.block[i];
  else if(!memcmp(g,&PathGuid,16))*p=a.path[i];
  else if(!memcmp(g,&FsGuid,16))*p=&a.fs[i];
  else return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
static UINTN IndexFile(EFI_FILE_PROTOCOL *file){return file==&a.file[1]?1:0;}
static EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *f){(void)f;closes++;return Is("close-error")?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs,EFI_FILE_PROTOCOL **root){
  *root=Is("invalid-root")?(VOID *)((UINTN)1<<60):&a.root[fs==&a.fs[1]?1:0];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *root,EFI_FILE_PROTOCOL **f,CHAR16 *path,UINT64 mode,UINT64 attrs){
  assert(!memcmp(path,WindowsPath,sizeof(WindowsPath))&&mode==EFI_FILE_MODE_READ&&!attrs);
  if(Is("missing-file")){*f=NULL;return EFI_NOT_FOUND;}
  UINTN i=root==&a.root[1]?1:0;positions[i]=0;*f=Is("invalid-file")?(VOID *)((UINTN)1<<60):&a.file[i];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *file,EFI_GUID *guid,UINTN *n,VOID *buf){
  (void)file;assert(!memcmp(guid,&InfoGuid,16)&&*n>=sizeof(EFI_FILE_INFO));
  EFI_FILE_INFO *f=buf;memset(f,0,sizeof(*f));*n=sizeof(*f);f->Size=*n;f->FileSize=sizeof(a.disk);
  if(Is("oversize"))f->FileSize=MAX_FILE+1ULL;
  if(Is("tiny-file"))f->FileSize=3;
  if(Is("directory"))f->Attribute=EFI_FILE_DIRECTORY;
  if(Is("bad-info-size"))f->Size=0;
  if(Is("file-grew")&&reads)f->FileSize++;
  return Is("info-error")?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *f,UINTN *n,VOID *buf){
  reads++;UINTN i=IndexFile(f),remain=sizeof(a.disk)-positions[i],take=*n<remain?*n:remain;
  if(Is("short-read")&&take)take--;
  if(Is("extra-byte")&&!remain){*(UINT8 *)buf=1;*n=1;return EFI_SUCCESS;}
  memcpy(buf,a.disk+positions[i],take);positions[i]+=take;*n=take;
  return Is("read-error")?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Load(BOOLEAN boot,EFI_HANDLE parent,EFI_DEVICE_PATH_PROTOCOL *path,VOID *source,UINTN bytes,EFI_HANDLE *out){
  loads++;assert(!boot&&parent==Gu.image&&source&&bytes==sizeof(a.disk));
  assert(!memcmp(source,a.disk,bytes));assert(path==W.full_path);
  *out=H(0x90);memset(a.loaded,0,sizeof(a.loaded));memcpy(a.loaded,a.disk,sizeof(a.disk));
  UINTN tail=4+sizeof(WindowsPath)+4;memcpy(a.loaded_path,(UINT8 *)path+W.volume.path_bytes-4,tail);
  a.li=(EFI_LOADED_IMAGE_PROTOCOL){.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION,.ParentHandle=Gu.image,
    .SystemTable=&table,.DeviceHandle=H(0x10),.FilePath=(VOID *)a.loaded_path,.ImageBase=a.loaded,
    .ImageSize=sizeof(a.loaded),.ImageCodeType=EfiLoaderCode,.ImageDataType=EfiLoaderData};
  if(Is("wrong-device"))a.li.DeviceHandle=H(0x20);
  if(Is("wrong-parent"))a.li.ParentHandle=H(0x20);
  if(Is("wrong-table"))a.li.SystemTable=NULL;
  if(Is("wrong-code-type"))a.li.ImageCodeType=EfiBootServicesCode;
  if(Is("wrong-data-type"))a.li.ImageDataType=EfiBootServicesData;
  if(Is("wrong-image-size"))a.li.ImageSize--;
  if(Is("wrong-loaded-path"))a.loaded_path[4]='Q';
  if(Is("load-options"))a.li.LoadOptionsSize=1;
  if(Is("null-load-image"))*out=NULL;
  if(Is("load-security"))return EFI_SECURITY_VIOLATION;
  if(Is("load-error"))return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unload(EFI_HANDLE h){assert(h==H(0x90));unloads++;return Is("unload-error")?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Start(EFI_HANDLE h,UINTN *bytes,CHAR16 **data){
  assert(h==H(0x90)&&bytes==NULL&&data==NULL&&mutated&&restored_calls==1&&!log_ready);
  starts++;return EFI_ABORTED;
}
static VOID Init(VOID){
  memset(&a,0,sizeof(a));memset(&bs,0,sizeof(bs));memset(&table,0,sizeof(table));
  bs.AllocatePool=Alloc;bs.FreePool=Release;bs.GetMemoryMap=Map;bs.LocateHandleBuffer=Handles;
  bs.HandleProtocol=Handle;bs.LoadImage=Load;bs.UnloadImage=Unload;bs.StartImage=Start;table.BootServices=&bs;
  Gu.bs=&bs;Gu.image=H(0x80);
  for(UINTN i=0;i<2;i++) {
    a.partition[i].revision=0x1000;a.partition[i].type=2;a.partition[i].system=1;
    a.partition[i].gpt.PartitionTypeGUID=EspGuid;a.partition[i].gpt.UniquePartitionGUID.Data1=(UINT32)(10+i);
    a.partition[i].gpt.StartingLBA=2048;a.partition[i].gpt.EndingLBA=4095;
    a.media[i]=(BLOCK_MEDIA){.id=1,.present=TRUE,.logical=TRUE,.block_size=512,.last_block=2047};
    a.block[i]=(BLOCK_IO){.revision=0x10000,.media=&a.media[i]};
    UINT8 prefix[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,0,0x1d,3,0x17,16,0,1,0,0,0,0,0,0,0,0,0,0,0};
    memcpy(a.path[i],prefix,sizeof(prefix));HARDDRIVE_DEVICE_PATH hd={0};
    hd.Header.Type=MEDIA_DEVICE_PATH;hd.Header.SubType=MEDIA_HARDDRIVE_DP;hd.Header.Length[0]=sizeof(hd);
    hd.PartitionNumber=1;hd.PartitionStart=2048;hd.PartitionSize=2048;
    memcpy(hd.Signature,&a.partition[i].gpt.UniquePartitionGUID,16);hd.MBRType=2;hd.SignatureType=2;
    memcpy(a.path[i]+sizeof(prefix),&hd,sizeof(hd));UINT8 end[]={127,255,4,0};memcpy(a.path[i]+sizeof(prefix)+sizeof(hd),end,4);
    a.fs[i].OpenVolume=OpenVolume;a.root[i].Open=Open;a.root[i].Close=Close;
    a.file[i].Close=Close;a.file[i].Read=Read;a.file[i].GetInfo=Info;
  }
  UINT8 *b=a.disk;b[0]='M';b[1]='Z';W32(b+60,0x80);memcpy(b+0x80,"PE\0\0",4);
  W16(b+0x84,0x8664);W16(b+0x86,1);W16(b+0x94,240);W16(b+0x96,2);
  UINT8 *o=b+0x98;W16(o,0x20b);W32(o+16,0x1000);W32(o+32,0x1000);W32(o+36,512);
  W32(o+56,8192);W32(o+60,512);W16(o+68,10);W32(o+108,16);
  UINT8 *s=o+240;memcpy(s,".text",5);W32(s+8,512);W32(s+12,0x1000);W32(s+16,512);W32(s+20,512);W32(s+36,0x60000020);
  b[512]=0xc3;
  if(Is("usb")){a.path[0][19]=MSG_USB_DP;a.path[0][20]=6;}
  if(Is("removable"))a.media[0].removable=TRUE;
  if(Is("no-media"))a.media[0].present=FALSE;
  if(Is("not-logical"))a.media[0].logical=FALSE;
  if(Is("bad-block-size"))a.media[0].block_size=1000;
  if(Is("partition-size"))a.media[0].last_block--;
  if(Is("not-esp"))a.partition[0].gpt.PartitionTypeGUID.Data1++;
  if(Is("not-gpt"))a.partition[0].type=1;
  if(Is("bad-partition-revision"))a.partition[0].revision=0;
  if(Is("bad-path-length"))a.path[0][2]=0;
  if(Is("path-multi-instance"))a.path[0][77]=1;
  if(Is("path-guid"))a.path[0][34+24]++;
  if(Is("pe-machine"))W16(b+0x84,0x14c);
  if(Is("pe-subsystem"))W16(o+68,3);
  if(Is("pe-entry"))W32(o+16,0x3000);
  if(Is("pe-raw-overrun"))W32(s+16,1024);
  if(Is("pe-virtual-overrun"))W32(s+8,0x2000);
  if(Is("pe-zero-sections"))W16(b+0x86,0);
  if(Is("pe-header-truncated"))W32(b+60,1023);
  if(Is("pe-image-overflow"))W32(o+56,0xfffff000);
  if(Is("pe-entry-bss")){W32(s+8,1024);W32(o+16,0x1300);}
  if(Is("pe-directory-overrun")){W32(o+112,8190);W32(o+116,8);}
  if(Is("pe-certificate-overrun")){W32(o+144,1024);W32(o+148,8);}
  if(Is("pe-overlap")){
    W16(b+0x86,2);memcpy(s+40,s,40);
  }
}
int main(int argc,char **argv){
  assert(argc==2);scenario=argv[1];Init();
  EFI_STATUS result=GuRehearsalWindowsPrepare(&table);
  BOOLEAN ready=Is("success")||Is("reentry")||Is("cancel")||Is("late-graph")||Is("late-media")||Is("late-loaded-code")||
    Is("late-loaded-path")||Is("late-volume-path")||Is("late-fault")||Is("before-mutation")||Is("log-still-open")||Is("unload-error");
  if(!ready){assert(result!=EFI_SUCCESS);assert(!starts);assert(!pools);if(Is("load-security")||Is("load-error"))assert(unloads==1);puts("refused with no StartImage: PASS");return 0;}
  assert(result==EFI_SUCCESS&&loads==1&&starts==0&&pools==2);
  assert(GuRehearsalWindowsPrepare(&table)==EFI_ALREADY_STARTED);
  if(Is("cancel")||Is("unload-error")){
    EFI_STATUS cancelled=GuRehearsalWindowsCancel();
    if(Is("unload-error")){assert(cancelled==EFI_DEVICE_ERROR&&pools==2);scenario="cancel";assert(GuRehearsalWindowsCancel()==EFI_SUCCESS);}
    else assert(cancelled==EFI_SUCCESS);
    assert(unloads>=1&&!pools&&!starts);puts("pre-mutation unload cleanup: PASS");return 0;
  }
  mutated=!Is("before-mutation");log_ready=Is("log-still-open");
  if(Is("late-media"))a.media[0].id++;
  if(Is("late-loaded-code"))a.loaded[100]++;
  if(Is("late-loaded-path"))a.loaded_path[4]++;
  if(Is("late-volume-path"))a.path[0][16]++;
  if(Is("late-fault"))Gu.fault=TRUE;
  result=GuRehearsalWindowsStart();
  if(Is("success")||Is("reentry")){assert(result==EFI_ABORTED&&starts==1);assert(GuRehearsalWindowsStart()==EFI_NOT_READY&&starts==1);}
  else assert(result!=EFI_SUCCESS&&starts==0);
  if(mutated)assert(GuRehearsalWindowsCancel()==EFI_ACCESS_DENIED);
  mutated=FALSE;W.started=FALSE;assert(GuRehearsalWindowsCancel()==EFI_SUCCESS);assert(!pools);
  puts("one-shot handoff and late validation: PASS");return 0;
}
