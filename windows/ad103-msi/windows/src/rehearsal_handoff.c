/* SPDX-License-Identifier: GPL-2.0-only */
#include "rehearsal_handoff.h"
#include "app_io.h"
#include "compat/compat.h"
#include "sha256.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Uefi/UefiGpt.h>
#if !((GU_REHEARSAL && !GU_ENABLE_UNLOCK && !GU_STAGED_UNLOCK) || \
      (GU_STAGED_UNLOCK && GU_ENABLE_UNLOCK && !GU_REHEARSAL))
#error Handoff requires an isolated rehearsal or staged unlock build
#endif
#if GU_STAGED_UNLOCK
#include "rehearsal_profile.h"
#if GU_STAGED_PROFILE_VERSION != 1
#error Staged handoff requires successful rehearsal evidence
#endif
#endif

extern BOOLEAN GuRehearsalMutated(VOID);
extern EFI_STATUS GuRehearsalRestored(VOID);

/* Minimal read-only ABI views. Layouts verified against EDK2 PartitionInfo.h
 * d2249b4 and BlockIo.h 9b42570. No block-I/O method is called. */
typedef struct {
  UINT32 revision,type;UINT8 system,reserved[7];EFI_PARTITION_ENTRY gpt;
} PARTITION_INFO;
typedef struct {
  UINT32 id;BOOLEAN removable,present,logical,readonly,caching;
  UINT32 block_size,alignment;EFI_LBA last_block;
} BLOCK_MEDIA;
typedef struct {UINT64 revision;BLOCK_MEDIA *media;VOID *reset,*read,*write,*flush;} BLOCK_IO;
_Static_assert(offsetof(PARTITION_INFO,gpt)==16,"partition protocol ABI");
_Static_assert(sizeof(BLOCK_MEDIA)==32,"block media ABI");
#define MAX_FILE (32U*1024U*1024U)
#define MAX_IMAGE (64U*1024U*1024U)
#define MAP_BYTES (256U*1024U)
#define MAX_PATH 4096U
static EFI_GUID FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID,
  ImageGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID,InfoGuid=EFI_FILE_INFO_ID;
static EFI_GUID PartitionGuid={0x8cf2f62c,0xbc9b,0x4821,{0x80,0x8d,0xec,0x9e,0xc4,0x21,0xa1,0xa0}},
  BlockGuid={0x964e5b21,0x6459,0x11d2,{0x8e,0x39,0,0xa0,0xc9,0x69,0x72,0x3b}},
  EspGuid={0xc12a7328,0xf81f,0x11d2,{0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b}};
static const CHAR16 WindowsPath[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
typedef struct {
  EFI_HANDLE handle;EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;PARTITION_INFO *part;BLOCK_IO *block;
  EFI_PARTITION_ENTRY gpt;BLOCK_MEDIA media;UINTN path_bytes;UINT8 path[MAX_PATH];
} VOLUME;
static struct {
  EFI_SYSTEM_TABLE *table;EFI_BOOT_SERVICES *bs;EFI_HANDLE self,image;
  VOLUME volume,scratch;EFI_LOADED_IMAGE_PROTOCOL loaded;
  EFI_DEVICE_PATH_PROTOCOL *full_path;UINTN full_path_bytes;
  UINT8 *map;UINTN map_bytes,stride;UINT8 loaded_hash[32];
  BOOLEAN ready,started,failed,busy;
} W;

static EFI_STATUS Free(VOID *p) {
  return p?W.bs->FreePool(p):EFI_SUCCESS;
}
static EFI_STATUS RefreshMap(VOID) {
  UINTN key=0;UINT32 version=0;W.map_bytes=MAP_BYTES;W.stride=0;
  EFI_STATUS s=W.bs->GetMemoryMap(&W.map_bytes,(VOID *)W.map,&key,&W.stride,&version);
  if(s!=EFI_SUCCESS||W.map_bytes>MAP_BYTES||W.stride<sizeof(EFI_MEMORY_DESCRIPTOR)||
     W.stride>256||W.map_bytes%W.stride||W.map_bytes/W.stride>4096||version!=1) {
    W.map_bytes=0;return s==EFI_SUCCESS?EFI_COMPROMISED_DATA:s;
  }
  return EFI_SUCCESS;
}
static BOOLEAN Readable(const VOID *p,UINTN bytes) {
  UINT64 at=(UINTN)p,end;
  if(!at||!bytes||bytes>~(UINT64)0-at||!W.map_bytes)return FALSE;
  end=at+bytes;
  while(at<end) {
    UINT64 next=0;UINTN matches=0;
    for(UINTN o=0;o<W.map_bytes;o+=W.stride) {
      EFI_MEMORY_DESCRIPTOR d;memcpy(&d,W.map+o,sizeof(d));
      if(d.NumberOfPages>(~(UINT64)0-d.PhysicalStart)/4096)return FALSE;
      UINT64 limit=d.PhysicalStart+d.NumberOfPages*4096;
      if(at<d.PhysicalStart||at>=limit)continue;
      if(++matches!=1||(d.Attribute&EFI_MEMORY_RP)||
         (d.Type!=EfiLoaderCode&&d.Type!=EfiLoaderData&&d.Type!=EfiBootServicesCode&&
          d.Type!=EfiBootServicesData&&d.Type!=EfiRuntimeServicesCode&&d.Type!=EfiRuntimeServicesData))return FALSE;
      next=limit;
    }
    if(matches!=1||next<=at)return FALSE;
    /* Do not leap over a descriptor that begins inside this interval: it
     * could overlap with a forbidden type or a second readable mapping. */
    for(UINTN o=0;o<W.map_bytes;o+=W.stride) {
      EFI_MEMORY_DESCRIPTOR d;memcpy(&d,W.map+o,sizeof(d));
      if(d.NumberOfPages&&d.PhysicalStart>at&&d.PhysicalStart<next)next=d.PhysicalStart;
    }
    at=next;
  }
  return TRUE;
}
static EFI_STATUS Interface(EFI_HANDLE h,EFI_GUID *guid,VOID **p,UINTN bytes) {
  *p=NULL;EFI_STATUS s=W.bs->HandleProtocol(h,guid,p);
  if(s==EFI_SUCCESS&&!Readable(*p,bytes))s=EFI_COMPROMISED_DATA;
  return s;
}
static UINT16 U16(const UINT8 *p){return (UINT16)p[0]|(UINT16)p[1]<<8;}
static UINT32 U32(const UINT8 *p){return (UINT32)U16(p)|(UINT32)U16(p+2)<<16;}
static BOOLEAN Span(UINTN off,UINTN bytes,UINTN limit){return off<=limit&&bytes<=limit-off;}
static BOOLEAN Power2(UINT32 n){return n&&!(n&(n-1));}
/* Validate a bounded on-disk AMD64 EFI application. Firmware LoadImage is
 * still responsible for complete loader/security validation. */
static EFI_STATUS Pe(const UINT8 *b,UINTN bytes,UINT32 *image_bytes) {
  if(bytes<512||b[0]!='M'||b[1]!='Z')return EFI_LOAD_ERROR;
  UINTN pe=U32(b+60);if(!Span(pe,24,bytes)||memcmp(b+pe,"PE\0\0",4)||U16(b+pe+4)!=0x8664)return EFI_LOAD_ERROR;
  UINTN sections=U16(b+pe+6),opt=pe+24,opts=U16(b+pe+20);
  if(!sections||sections>32||opts<240||!Span(opt,opts,bytes)||U16(b+opt)!=0x20b||
     !(U16(b+pe+22)&2)||U16(b+opt+68)!=10)return EFI_LOAD_ERROR;
  UINT32 entry=U32(b+opt+16),align=U32(b+opt+32),file_align=U32(b+opt+36),
    image=U32(b+opt+56),headers=U32(b+opt+60),dirs=U32(b+opt+108);
  UINTN table=opt+opts;
  if(!Power2(align)||align<4096||!Power2(file_align)||file_align<512||file_align>65536||align<file_align||
     !image||image>MAX_IMAGE||image%align||!headers||headers>bytes||headers%file_align||headers>image||
     !Span(table,sections*40,headers)||dirs>16||112+(UINTN)dirs*8>opts)return EFI_LOAD_ERROR;
  for(UINTN i=0;i<dirs;i++) {
    UINT32 address=U32(b+opt+112+i*8),size=U32(b+opt+116+i*8);
    if((!address)!=(!size))return EFI_LOAD_ERROR;
    if(size&&((i==4&&(address%8||!Span(address,size,bytes)))||
              (i!=4&&!Span(address,size,image))))return EFI_LOAD_ERROR;
  }
  BOOLEAN entry_ok=FALSE;
  for(UINTN i=0;i<sections;i++) {
    const UINT8 *s=b+table+i*40;UINT32 virtual_bytes=U32(s+8),rva=U32(s+12),raw_bytes=U32(s+16),raw=U32(s+20);
    UINT32 size=virtual_bytes>raw_bytes?virtual_bytes:raw_bytes;
    if(!size||rva<headers||rva%align||!Span(rva,size,image)||
       (raw_bytes&&(raw<headers||raw%file_align||raw_bytes%file_align||!Span(raw,raw_bytes,bytes))))return EFI_LOAD_ERROR;
    for(UINTN j=0;j<i;j++) {
      const UINT8 *t=b+table+j*40;UINT32 v=U32(t+8),rb=U32(t+16),sz=v>rb?v:rb,rv=U32(t+12),ra=U32(t+20);
      if(rva<rv+sz&&rv<rva+size)return EFI_LOAD_ERROR;
      if(raw_bytes&&rb&&raw<ra+rb&&ra<raw+raw_bytes)return EFI_LOAD_ERROR;
    }
    if(entry>=rva&&entry-rva<raw_bytes&&(U32(s+36)&0x20000000U))entry_ok=TRUE;
  }
  if(!entry_ok)return EFI_LOAD_ERROR;
  *image_bytes=image;return EFI_SUCCESS;
}
/* Require an internal PCI SATA/NVMe partition path, one GPT HD node and a
 * single end node. USB/network/removable/opaque paths are deliberately refused. */
static EFI_STATUS Volume(EFI_HANDLE h,VOLUME *v) {
  memset(v,0,sizeof(*v));v->handle=h;
  EFI_STATUS s=Interface(h,&PartitionGuid,(VOID **)&v->part,sizeof(*v->part));
  if(s==EFI_UNSUPPORTED||s==EFI_NOT_FOUND)return EFI_NOT_FOUND;
  if(s!=EFI_SUCCESS)return s;
  if(v->part->type!=2||!v->part->system||memcmp(&v->part->gpt.PartitionTypeGUID,&EspGuid,16))return EFI_NOT_FOUND;
  if(v->part->revision!=0x1000||v->part->system!=1)return EFI_UNSUPPORTED;
  v->gpt=v->part->gpt;
  EFI_GUID zero={0};if(!memcmp(&v->gpt.UniquePartitionGUID,&zero,16)||!v->gpt.StartingLBA||v->gpt.EndingLBA<v->gpt.StartingLBA)return EFI_COMPROMISED_DATA;
  s=Interface(h,&BlockGuid,(VOID **)&v->block,sizeof(*v->block));if(s!=EFI_SUCCESS)return s;
  if(v->block->revision<0x10000||!Readable(v->block->media,sizeof(BLOCK_MEDIA)))return EFI_COMPROMISED_DATA;
  v->media=*v->block->media;
  if(v->media.removable)return EFI_NOT_FOUND;
  if(!v->media.present||!v->media.logical||!Power2(v->media.block_size)||v->media.block_size<512||v->media.block_size>65536||
     v->media.last_block!=v->gpt.EndingLBA-v->gpt.StartingLBA)return EFI_UNSUPPORTED;
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;s=Interface(h,&PathGuid,(VOID **)&p,4);if(s!=EFI_SUCCESS)return s;
  UINTN offset=0;BOOLEAN pci=FALSE,transport=FALSE,hd=FALSE,end=FALSE;
  for(UINTN nodes=0;nodes<128;nodes++) {
    if(offset>MAX_PATH-4||!Readable((UINT8 *)p+offset,4))return EFI_COMPROMISED_DATA;
    const UINT8 *node=(const UINT8 *)p+offset;UINTN n=U16(node+2);
    if(n<4||n>MAX_PATH-offset||!Readable(node,n))return EFI_COMPROMISED_DATA;
    if(node[0]==END_DEVICE_PATH_TYPE) {
      if(node[1]!=END_ENTIRE_DEVICE_PATH_SUBTYPE||n!=4||!hd)return EFI_UNSUPPORTED;
      memcpy(v->path+offset,node,n);offset+=n;end=TRUE;break;
    }
    if(hd)return EFI_UNSUPPORTED;
    if(node[0]==MESSAGING_DEVICE_PATH) {
      if(node[1]==MSG_USB_DP||node[1]==MSG_USB_CLASS_DP||node[1]==MSG_USB_WWID_DP)return EFI_NOT_FOUND;
      if(!pci||transport||!((node[1]==MSG_SATA_DP&&n==10)||(node[1]==MSG_NVME_NAMESPACE_DP&&n==16)))return EFI_UNSUPPORTED;
      transport=TRUE;
    } else if(node[0]==HARDWARE_DEVICE_PATH&&node[1]==HW_PCI_DP&&n==6&&!transport) {
      pci=TRUE;
    } else if(node[0]==ACPI_DEVICE_PATH&&(node[1]==ACPI_DP||node[1]==ACPI_EXTENDED_DP)&&!pci) {
      if((node[1]==ACPI_DP&&n!=12)||(node[1]==ACPI_EXTENDED_DP&&n<19))return EFI_UNSUPPORTED;
    } else if(node[0]==MEDIA_DEVICE_PATH&&node[1]==MEDIA_HARDDRIVE_DP&&n==sizeof(HARDDRIVE_DEVICE_PATH)&&transport) {
      HARDDRIVE_DEVICE_PATH d;memcpy(&d,node,sizeof(d));
      if(d.MBRType!=MBR_TYPE_EFI_PARTITION_TABLE_HEADER||d.SignatureType!=SIGNATURE_TYPE_GUID||!d.PartitionNumber||
         d.PartitionStart!=v->gpt.StartingLBA||d.PartitionSize!=v->media.last_block+1||
         memcmp(d.Signature,&v->gpt.UniquePartitionGUID,16))return EFI_COMPROMISED_DATA;
      hd=TRUE;
    } else return EFI_UNSUPPORTED;
    memcpy(v->path+offset,node,n);offset+=n;
  }
  if(!end)return EFI_COMPROMISED_DATA;
  v->path_bytes=offset;
  s=Interface(h,&FsGuid,(VOID **)&v->fs,sizeof(*v->fs));
  if(s==EFI_SUCCESS&&!v->fs->OpenVolume)s=EFI_UNSUPPORTED;
  return s;
}
static BOOLEAN SameVolume(const VOLUME *a,const VOLUME *b) {
  return a->handle==b->handle&&a->fs==b->fs&&a->part==b->part&&a->block==b->block&&
    !memcmp(&a->gpt,&b->gpt,sizeof(a->gpt))&&a->media.id==b->media.id&&
    a->media.block_size==b->media.block_size&&a->media.last_block==b->media.last_block&&
    a->path_bytes==b->path_bytes&&!memcmp(a->path,b->path,a->path_bytes);
}
static EFI_STATUS FileSize(EFI_FILE_PROTOCOL *file,UINTN *bytes) {
  union {UINT64 align;UINT8 bytes[1024];} storage;UINTN n=sizeof(storage);
  EFI_STATUS s=file->GetInfo(file,&InfoGuid,&n,storage.bytes);
  if(s!=EFI_SUCCESS)return s;
  EFI_FILE_INFO *info=(VOID *)storage.bytes;
  if(n<offsetof(EFI_FILE_INFO,FileName)+2||n>sizeof(storage)||info->Size!=n||info->FileSize<512||
     info->FileSize>MAX_FILE||(info->Attribute&EFI_FILE_DIRECTORY))return EFI_BAD_BUFFER_SIZE;
  *bytes=(UINTN)info->FileSize;return EFI_SUCCESS;
}
static EFI_STATUS ReadCandidate(const VOLUME *v,UINT8 **data,UINTN *bytes,UINT32 *image_bytes) {
  EFI_FILE_PROTOCOL *root=NULL,*file=NULL;BOOLEAN root_valid=FALSE,file_valid=FALSE;*data=NULL;*bytes=0;
  EFI_STATUS s=v->fs->OpenVolume(v->fs,&root);
  EFI_STATUS mapped=RefreshMap();
  if(mapped==EFI_SUCCESS&&Readable(root,sizeof(*root)))root_valid=TRUE;
  if(s==EFI_SUCCESS&&mapped!=EFI_SUCCESS)s=mapped;
  if(s==EFI_SUCCESS&&(!root_valid||!root->Open||!root->Close))s=EFI_COMPROMISED_DATA;
  if(s==EFI_SUCCESS) {
    s=root->Open(root,&file,(CHAR16 *)WindowsPath,EFI_FILE_MODE_READ,0);
    mapped=RefreshMap();
    if(mapped==EFI_SUCCESS&&Readable(file,sizeof(*file)))file_valid=TRUE;
    if(s==EFI_SUCCESS&&mapped!=EFI_SUCCESS)s=mapped;
    if(s==EFI_SUCCESS&&(!file_valid||!file->Read||!file->Close||!file->GetInfo))s=EFI_COMPROMISED_DATA;
  }
  if(s==EFI_SUCCESS)s=FileSize(file,bytes);
  if(s==EFI_SUCCESS){*data=GuAlloc(*bytes);if(!*data)s=EFI_OUT_OF_RESOURCES;}
  for(UINTN done=0;s==EFI_SUCCESS&&done<*bytes;) {
    UINTN n=*bytes-done;if(n>1048576)n=1048576;UINTN expected=n;
    s=file->Read(file,&n,*data+done);if(s==EFI_SUCCESS&&n!=expected)s=EFI_DEVICE_ERROR;
    if(s==EFI_SUCCESS)done+=n;
  }
  if(s==EFI_SUCCESS){UINT8 tail;UINTN n=1;s=file->Read(file,&n,&tail);if(s==EFI_SUCCESS&&n)s=EFI_MEDIA_CHANGED;}
  if(s==EFI_SUCCESS){UINTN after=0;s=FileSize(file,&after);if(s==EFI_SUCCESS&&after!=*bytes)s=EFI_MEDIA_CHANGED;}
  if(file_valid&&file->Close){EFI_STATUS closed=file->Close(file);if(closed!=EFI_SUCCESS)s=closed;}
  if(root_valid&&root->Close){EFI_STATUS closed=root->Close(root);if(closed!=EFI_SUCCESS)s=closed;}
  if(s==EFI_SUCCESS)s=Pe(*data,*bytes,image_bytes);
  if(s!=EFI_SUCCESS&&*data){EFI_STATUS released=Free(*data);*data=NULL;if(released!=EFI_SUCCESS)s=released;}
  return s;
}
static EFI_STATUS MakePath(VOID) {
  UINTN prefix=W.volume.path_bytes-4,node=4+sizeof(WindowsPath),total=prefix+node+4;
  UINT8 *b=GuAlloc(total);if(!b)return EFI_OUT_OF_RESOURCES;
  memcpy(b,W.volume.path,prefix);b[prefix]=MEDIA_DEVICE_PATH;b[prefix+1]=MEDIA_FILEPATH_DP;
  b[prefix+2]=(UINT8)node;b[prefix+3]=(UINT8)(node>>8);memcpy(b+prefix+4,WindowsPath,sizeof(WindowsPath));
  b[prefix+node]=END_DEVICE_PATH_TYPE;b[prefix+node+1]=END_ENTIRE_DEVICE_PATH_SUBTYPE;b[prefix+node+2]=4;
  W.full_path=(VOID *)b;W.full_path_bytes=total;return EFI_SUCCESS;
}
static BOOLEAN LoadedPath(const EFI_DEVICE_PATH_PROTOCOL *p) {
  UINTN node=4+sizeof(WindowsPath),size=node+4;
  return Readable(p,size)&&!memcmp(p,(UINT8 *)W.full_path+W.volume.path_bytes-4,size);
}
static EFI_STATUS Loaded(BOOLEAN first,UINT32 image_bytes) {
  EFI_LOADED_IMAGE_PROTOCOL *li=NULL;EFI_STATUS s=Interface(W.image,&ImageGuid,(VOID **)&li,sizeof(*li));
  if(s!=EFI_SUCCESS)return s;
  if(li->Revision!=EFI_LOADED_IMAGE_PROTOCOL_REVISION||li->ParentHandle!=W.self||li->SystemTable!=W.table||
     li->DeviceHandle!=W.volume.handle||li->ImageCodeType!=EfiLoaderCode||li->ImageDataType!=EfiLoaderData||
     !li->ImageBase||!li->ImageSize||li->ImageSize>MAX_IMAGE||li->LoadOptionsSize||li->LoadOptions||
     !LoadedPath(li->FilePath)||!Readable(li->ImageBase,(UINTN)li->ImageSize))return EFI_COMPROMISED_DATA;
  if(first) {
    if(li->ImageSize!=image_bytes)return EFI_COMPROMISED_DATA;
    W.loaded=*li;GuSha256(li->ImageBase,(UINTN)li->ImageSize,W.loaded_hash);
  } else {
    if(li->ImageBase!=W.loaded.ImageBase||li->ImageSize!=W.loaded.ImageSize||li->FilePath!=W.loaded.FilePath||
       li->Unload!=W.loaded.Unload)return EFI_COMPROMISED_DATA;
    UINT8 hash[32];GuSha256(li->ImageBase,(UINTN)li->ImageSize,hash);
    if(memcmp(hash,W.loaded_hash,32))return EFI_CRC_ERROR;
  }
  return EFI_SUCCESS;
}
EFI_STATUS GuRehearsalWindowsCancel(VOID) {
  if(GuRehearsalMutated()||W.started||W.busy)return EFI_ACCESS_DENIED;
  if(!W.bs)return EFI_SUCCESS;
  EFI_STATUS s=EFI_SUCCESS;W.busy=TRUE;
  if(W.image){s=W.bs->UnloadImage(W.image);if(s!=EFI_SUCCESS){W.failed=TRUE;W.ready=FALSE;W.busy=FALSE;return s;}W.image=NULL;}
  EFI_STATUS freed=Free(W.full_path);if(s==EFI_SUCCESS)s=freed;
  freed=Free(W.map);if(s==EFI_SUCCESS)s=freed;
  memset(&W,0,sizeof(W));return s;
}
EFI_STATUS GuRehearsalWindowsPrepare(EFI_SYSTEM_TABLE *table) {
  if(W.bs||W.ready||W.started)return EFI_ALREADY_STARTED;
  if(!table||!table->BootServices||table->BootServices!=Gu.bs||!Gu.image||Gu.fault||Gu.started||
     Gu.armed||GuRehearsalMutated())return EFI_INVALID_PARAMETER;
  EFI_BOOT_SERVICES *bs=table->BootServices;
  if(!bs->LocateHandleBuffer||!bs->HandleProtocol||!bs->LoadImage||!bs->StartImage||!bs->UnloadImage||
     !bs->AllocatePool||!bs->FreePool||!bs->GetMemoryMap)return EFI_UNSUPPORTED;
  W.bs=bs;W.table=table;W.self=Gu.image;W.busy=TRUE;W.map=GuAlloc(MAP_BYTES);
  EFI_STATUS s=W.map?RefreshMap():EFI_OUT_OF_RESOURCES;
  EFI_HANDLE *handles=NULL;UINTN count=0,found=0;UINT8 *chosen=NULL;UINTN chosen_bytes=0;UINT32 image_bytes=0;
  if(s==EFI_SUCCESS)s=bs->LocateHandleBuffer(ByProtocol,&FsGuid,NULL,&count,&handles);
  if(s==EFI_SUCCESS&&(!handles||!count||count>256))s=EFI_BAD_BUFFER_SIZE;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    if(!handles[i]){s=EFI_COMPROMISED_DATA;break;}
    for(UINTN j=0;j<i;j++)if(handles[j]==handles[i])s=EFI_COMPROMISED_DATA;
    if(s!=EFI_SUCCESS)break;
    s=Volume(handles[i],&W.scratch);
    if(s==EFI_NOT_FOUND){s=EFI_SUCCESS;continue;}if(s!=EFI_SUCCESS)break;
    UINT8 *data=NULL;UINTN bytes=0;UINT32 size=0;s=ReadCandidate(&W.scratch,&data,&bytes,&size);
    if(s==EFI_NOT_FOUND){s=EFI_SUCCESS;continue;}if(s!=EFI_SUCCESS)break;
    found++;
    if(found!=1){s=Free(data);if(s==EFI_SUCCESS)s=EFI_NO_MAPPING;break;}
    W.volume=W.scratch;chosen=data;chosen_bytes=bytes;image_bytes=size;
  }
  EFI_STATUS freed=Free(handles);if(s==EFI_SUCCESS)s=freed;
  if(s==EFI_SUCCESS&&found!=1)s=EFI_NOT_FOUND;
#if GU_STAGED_UNLOCK
  if(s==EFI_SUCCESS&&(chosen_bytes!=GU_STAGED_WINDOWS_PRELOAD_BYTES ||
    !GuHashMatches(chosen,chosen_bytes,GU_STAGED_WINDOWS_PRELOAD_SHA256)))s=EFI_CRC_ERROR;
#endif
  if(s==EFI_SUCCESS)s=MakePath();
  if(s==EFI_SUCCESS) {
    UINT8 hash[32];char hex[65];static const char digit[]="0123456789abcdef";
    GuSha256(chosen,chosen_bytes,hash);for(UINTN i=0;i<32;i++){hex[i*2]=digit[hash[i]>>4];hex[i*2+1]=digit[hash[i]&15];}hex[64]=0;
    GuLog("WINDOWS_PRELOAD internal_gpt_esp=1 candidates=1 bytes=%zu sha256=%s same_boot_pin=1\n",(size_t)chosen_bytes,hex);
    if(!GuLogReady()||Gu.fault)s=EFI_DEVICE_ERROR;
  }
  if(s==EFI_SUCCESS)s=bs->LoadImage(FALSE,W.self,W.full_path,chosen,chosen_bytes,&W.image);
  freed=Free(chosen);if(s==EFI_SUCCESS)s=freed;
  if(s==EFI_SUCCESS&&!W.image)s=EFI_COMPROMISED_DATA;
  if(s==EFI_SUCCESS)s=RefreshMap();
  if(s==EFI_SUCCESS){s=Volume(W.volume.handle,&W.scratch);if(s==EFI_SUCCESS&&!SameVolume(&W.volume,&W.scratch))s=EFI_MEDIA_CHANGED;}
  if(s==EFI_SUCCESS)s=Loaded(TRUE,image_bytes);
  if(s==EFI_SUCCESS&&(!GuLogReady()||Gu.fault))s=EFI_DEVICE_ERROR;
  W.busy=FALSE;
  if(s!=EFI_SUCCESS){EFI_STATUS closed=GuRehearsalWindowsCancel();return closed!=EFI_SUCCESS?closed:s;}
  W.ready=TRUE;return EFI_SUCCESS;
}
EFI_STATUS GuRehearsalWindowsStart(VOID) {
  if(!W.ready||W.failed||W.started||W.busy||Gu.fault||GuLogReady()||!GuRehearsalMutated())return EFI_NOT_READY;
  W.busy=TRUE;
  EFI_STATUS s=RefreshMap();
  if(s==EFI_SUCCESS)s=Volume(W.volume.handle,&W.scratch);
  if(s==EFI_SUCCESS&&!SameVolume(&W.volume,&W.scratch))s=EFI_MEDIA_CHANGED;
  if(s==EFI_SUCCESS)s=Loaded(FALSE,0);
  if(s==EFI_SUCCESS)s=GuRehearsalRestored();
  if(s!=EFI_SUCCESS){W.busy=FALSE;return s;}
  /* No logging, allocations, controller calls or reselection after the final
   * restored check. Start is one-shot even if firmware returns an error. */
  W.started=TRUE;W.ready=FALSE;W.busy=FALSE;
  return W.bs->StartImage(W.image,NULL,NULL);
}
