/* SPDX-License-Identifier: GPL-2.0-only */
/* Logs only on the pinned ESP. Assets and Windows are read-only. No boot variables. */
#include "boot_io.h"
#include "boot_profile.h"
#include "compat/compat.h"
#include "format.h"
#include "profile.h"
#include "sha256.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

static EFI_SYSTEM_TABLE *Table;
static EFI_HANDLE Self,Windows,OwnVolume;
static EFI_FILE_PROTOCOL *Root,*Log;
static BOOLEAN Silent;
static EFI_GUID LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID,FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID,InfoGuid=EFI_FILE_INFO_ID;
typedef struct {EFI_GUID guid;UINT32 number;UINT64 start,sectors;} BOOT_PARTITION;
static const BOOT_PARTITION WindowsPartition={GU_WINDOWS_PARTITION_INITIALIZER,1,2048,204800};
static const BOOT_PARTITION AssetPartition={GU_ASSET_PARTITION_INITIALIZER,3,239616,3998527488ULL};
static const CHAR16 WindowsName[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

static BOOLEAN Inspect(const EFI_DEVICE_PATH_PROTOCOL *p,UINTN *size,BOOLEAN *usb,const HARDDRIVE_DEVICE_PATH **partition) {
  UINTN at=0;*usb=FALSE;*partition=NULL;
  if(!p)return FALSE;
  for(UINTN nodes=0;nodes<64&&at+4<=4096;nodes++) {
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||n>4096-at)return FALSE;
    if(p->Type==END_DEVICE_PATH_TYPE) {
      if(n!=4||p->SubType!=END_ENTIRE_DEVICE_PATH_SUBTYPE)return FALSE;
      *size=at+4;return TRUE;
    }
    /* A filesystem partition path ends at its single hard-drive node. */
    if(*partition||p->Type<HARDWARE_DEVICE_PATH||p->Type>BBS_DEVICE_PATH)return FALSE;
    if(p->Type==MESSAGING_DEVICE_PATH&&(p->SubType==MSG_USB_DP||
       p->SubType==MSG_USB_CLASS_DP||p->SubType==MSG_USB_WWID_DP))*usb=TRUE;
    if(p->Type==MEDIA_DEVICE_PATH) {
      if(p->SubType!=MEDIA_HARDDRIVE_DP||n!=sizeof(HARDDRIVE_DEVICE_PATH))return FALSE;
      *partition=(const VOID *)p;
    }
    at+=n;p=(const VOID *)((const UINT8 *)p+n);
  }
  return FALSE;
}
static EFI_STATUS FindVolume(const BOOT_PARTITION *wanted,EFI_HANDLE *volume,EFI_DEVICE_PATH_PROTOCOL **path,UINTN *path_bytes) {
  static const EFI_GUID zero={0};
  EFI_BOOT_SERVICES *b=Table->BootServices;EFI_HANDLE *h=NULL;UINTN count=0,matches=0;
  *volume=NULL;*path=NULL;*path_bytes=0;
  if(!memcmp(&wanted->guid,&zero,sizeof(zero)))return EFI_UNSUPPORTED;
  EFI_STATUS s=b->LocateHandleBuffer(ByProtocol,&FsGuid,NULL,&count,&h);
  if(s==EFI_SUCCESS&&(!h||!count||count>128))s=EFI_UNSUPPORTED;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_DEVICE_PATH_PROTOCOL *p=NULL;const HARDDRIVE_DEVICE_PATH *d=NULL;UINTN bytes=0;BOOLEAN usb=FALSE;
    s=b->HandleProtocol(h[i],&PathGuid,(VOID **)&p);
    if(s!=EFI_SUCCESS||!Inspect(p,&bytes,&usb,&d)){s=EFI_UNSUPPORTED;break;}
    if(!d||memcmp(d->Signature,&wanted->guid,16))continue;
    /* Count GUID identity before geometry: an inconsistent duplicate is unsafe. */
    if(++matches!=1||usb||d->MBRType!=2||d->SignatureType!=2||
       d->PartitionNumber!=wanted->number||d->PartitionStart!=wanted->start||d->PartitionSize!=wanted->sectors) {
      s=EFI_COMPROMISED_DATA;break;
    }
    *volume=h[i];*path=p;*path_bytes=bytes;
  }
  if(h)b->FreePool(h);
  return s!=EFI_SUCCESS?s:matches==1?EFI_SUCCESS:EFI_NOT_FOUND;
}
VOID GuBootSay(const CHAR16 *text) {
  if(!Silent&&Table&&Table->ConOut&&Table->ConOut->OutputString)
    Table->ConOut->OutputString(Table->ConOut,(CHAR16 *)text);
}
VOID GuBootSilence(VOID){Silent=TRUE;}
VOID GuLog(const char *format,...) {
  CHAR8 buffer[2048];va_list args;va_start(args,format);
  size_t n=GuFormatV(buffer,sizeof(buffer),format,args);va_end(args);
  if(n==(size_t)-1||n>=sizeof(buffer)||!Log){Gu.fault=TRUE;return;}
  UINTN bytes=n;EFI_STATUS s=Log->Write(Log,&bytes,buffer);
  if(s!=EFI_SUCCESS||bytes!=n||Log->Flush(Log)!=EFI_SUCCESS)Gu.fault=TRUE;
}
VOID GuLogText(const char *text){GuLog("%s",text);}
BOOLEAN GuBootLogOpen(VOID){return Log!=NULL;}
EFI_STATUS GuBootOpen(EFI_HANDLE image,EFI_SYSTEM_TABLE *t) {
  if(Table)return EFI_ALREADY_STARTED;
  if(!image||!t||!t->BootServices)return EFI_INVALID_PARAMETER;
  Table=t;Self=image;
  EFI_BOOT_SERVICES *b=t->BootServices;EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;EFI_DEVICE_PATH_PROTOCOL *path=NULL;
  EFI_HANDLE volume=NULL;UINTN length=0;
  if(!b->HandleProtocol||!b->LocateHandleBuffer||!b->FreePool)return EFI_UNSUPPORTED;
  EFI_STATUS s=b->HandleProtocol(image,&LoadedGuid,(VOID **)&loaded);
  if(s!=EFI_SUCCESS||!loaded||!loaded->DeviceHandle)return EFI_UNSUPPORTED;
  OwnVolume=loaded->DeviceHandle;s=FindVolume(&WindowsPartition,&volume,&path,&length);
  if(s!=EFI_SUCCESS||volume!=OwnVolume)return EFI_ACCESS_DENIED;
  s=b->HandleProtocol(OwnVolume,&FsGuid,(VOID **)&fs);if(s!=EFI_SUCCESS||!fs||!fs->OpenVolume)return EFI_UNSUPPORTED;
  s=fs->OpenVolume(fs,&Root);if(s!=EFI_SUCCESS||!Root)return EFI_DEVICE_ERROR;
  EFI_FILE_PROTOCOL *dir=NULL;
  s=Root->Open(Root,&dir,L"\\EFI\\4070S-PROBE\\LOGS",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
  if(s!=EFI_SUCCESS||!dir){s=EFI_DEVICE_ERROR;goto failed;}
  for(UINTN slot=1;slot<=9999;slot++) {
    CHAR16 name[]=L"RUN0000.TXT";UINTN number=slot;
    for(UINTN at=7;at>3;){at--;name[at]=(CHAR16)('0'+number%10);number/=10;}
    EFI_FILE_PROTOCOL *existing=NULL;s=dir->Open(dir,&existing,name,EFI_FILE_MODE_READ,0);
    if(s==EFI_SUCCESS&&existing){s=existing->Close(existing);if(s!=EFI_SUCCESS)break;continue;}
    if(s!=EFI_NOT_FOUND)break;
    s=dir->Open(dir,&Log,name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);break;
  }
  EFI_STATUS closed=dir->Close(dir);
  if(s==EFI_SUCCESS&&!Log)s=EFI_OUT_OF_RESOURCES;
  if(s==EFI_SUCCESS)s=closed;
  if(s==EFI_SUCCESS)s=b->SetWatchdogTimer?b->SetWatchdogTimer(0,0,0,NULL):EFI_UNSUPPORTED;
  if(s==EFI_SUCCESS)return s;
failed:
  (VOID)GuBootClose();return s;
}
static EFI_STATUS ReadFile(EFI_FILE_PROTOCOL *root,const CHAR16 *name,UINTN expected,VOID **data) {
  *data=NULL;EFI_FILE_PROTOCOL *f=NULL;UINT64 info_words[128];UINTN info_bytes=sizeof(info_words);
  EFI_STATUS s=root->Open(root,&f,(CHAR16 *)name,EFI_FILE_MODE_READ,0);
  if(s!=EFI_SUCCESS||!f)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  s=f->GetInfo(f,&InfoGuid,&info_bytes,info_words);
  EFI_FILE_INFO *info=(VOID *)info_words;
  if(s==EFI_SUCCESS&&(info_bytes<sizeof(EFI_FILE_INFO)||info_bytes>sizeof(info_words)||
     info->FileSize!=expected||(info->Attribute&EFI_FILE_DIRECTORY)))s=EFI_COMPROMISED_DATA;
  VOID *p=NULL;
  if(s==EFI_SUCCESS){p=GuAlloc(expected);if(!p)s=EFI_OUT_OF_RESOURCES;}
  for(UINTN at=0;s==EFI_SUCCESS&&at<expected;) {
    UINTN n=expected-at;if(n>1048576)n=1048576;UINTN wanted=n;
    s=f->Read(f,&n,(UINT8 *)p+at);if(s==EFI_SUCCESS&&n!=wanted)s=EFI_DEVICE_ERROR;at+=n;
  }
  if(s==EFI_SUCCESS){UINT8 tail;UINTN n=1;s=f->Read(f,&n,&tail);if(s==EFI_SUCCESS&&n)s=EFI_COMPROMISED_DATA;}
  EFI_STATUS closed=f->Close(f);if(s==EFI_SUCCESS)s=closed;
  if(s!=EFI_SUCCESS){GuFree(p);return s;}*data=p;return EFI_SUCCESS;
}
EFI_STATUS GuBootLoadAssets(VOID) {
  static const CHAR16 *names[]={L"\\EFI\\4070S-PROBE\\ASSETS\\booter_load_ad102.bin",L"\\EFI\\4070S-PROBE\\ASSETS\\v67-ad102-610.43.03-four-plm.bin",
    GU_GSP_PATH,L"\\EFI\\4070S-PROBE\\ASSETS\\gsp_boot_ad102.bin",L"\\EFI\\4070S-PROBE\\ASSETS\\gsp_boot_desc_ad102.bin"};
  static const char *full[]={"gpu-unlock/610.43.03/booter_load_ad102.bin","gpu-unlock/610.43.03/v67-ad102-610.43.03-four-plm.bin",
    "gpu-unlock/610.43.03/gsp_ga10x.bin","gpu-unlock/610.43.03/gsp_boot_ad102.bin","gpu-unlock/610.43.03/gsp_boot_desc_ad102.bin"};
  static const UINTN sizes[]={57672,64000,84277400,36864,84};
  if(!Root||!Table||Gu.started)return EFI_NOT_READY;
  EFI_HANDLE volume=NULL;EFI_DEVICE_PATH_PROTOCOL *path=NULL;UINTN path_bytes=0;
  EFI_STATUS s=FindVolume(&AssetPartition,&volume,&path,&path_bytes);
  if(s!=EFI_SUCCESS||volume==OwnVolume)return s==EFI_SUCCESS?EFI_UNSUPPORTED:s;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;EFI_FILE_PROTOCOL *root=NULL;
  s=Table->BootServices->HandleProtocol(volume,&FsGuid,(VOID **)&fs);
  if(s!=EFI_SUCCESS||!fs||!fs->OpenVolume)return EFI_UNSUPPORTED;
  s=fs->OpenVolume(fs,&root);if(s!=EFI_SUCCESS||!root)return EFI_DEVICE_ERROR;
  for(UINTN i=0;i<5;i++) {
    VOID *p=NULL;s=ReadFile(i==2?root:Root,names[i],sizes[i],&p);
    if(s!=EFI_SUCCESS)break;
    GuAssets[i].name=full[i];GuAssets[i].firmware=(struct firmware){sizes[i],p};
  }
  EFI_STATUS closed=root->Close(root);if(s==EFI_SUCCESS)s=closed;
  if(s==EFI_SUCCESS&&!GuAd104AssetsValid())s=EFI_CRC_ERROR;
  if(s!=EFI_SUCCESS)for(UINTN i=0;i<5;i++){GuFree((VOID *)GuAssets[i].firmware.data);memset(&GuAssets[i],0,sizeof(GuAssets[i]));}
  return s;
}
EFI_STATUS GuBootLoadWindows(VOID) {
  if(Windows||!Table||Gu.started)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Table->BootServices;EFI_HANDLE volume=NULL,staged=NULL;UINTN path_bytes=0;
  EFI_DEVICE_PATH_PROTOCOL *base=NULL;
  EFI_STATUS s=FindVolume(&WindowsPartition,&volume,&base,&path_bytes);
  if(s!=EFI_SUCCESS||volume!=OwnVolume)return EFI_NOT_FOUND;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;EFI_FILE_PROTOCOL *root=NULL;
  s=b->HandleProtocol(volume,&FsGuid,(VOID **)&fs);
  if(s==EFI_SUCCESS&&fs&&fs->OpenVolume)s=fs->OpenVolume(fs,&root);else s=EFI_UNSUPPORTED;
  if(s!=EFI_SUCCESS||!root)return EFI_DEVICE_ERROR;
  VOID *bytes=NULL;s=ReadFile(root,WindowsName,3086680,&bytes);
  EFI_STATUS closed=root->Close(root);if(s==EFI_SUCCESS)s=closed;
  if(s==EFI_SUCCESS&&!GuHashMatches(bytes,3086680,"2d60a2203865122dad22772136b3d905167d9dcf926d602bb5f48b3b6668abec"))s=EFI_CRC_ERROR;
  UINTN node_bytes=4+sizeof(WindowsName),total=path_bytes-4+node_bytes+4;
  UINT8 *path=NULL;
  if(s==EFI_SUCCESS){path=GuAlloc(total);if(!path)s=EFI_OUT_OF_RESOURCES;}
  if(s==EFI_SUCCESS) {
    memcpy(path,base,path_bytes-4);UINT8 *node=path+path_bytes-4;
    node[0]=MEDIA_DEVICE_PATH;node[1]=MEDIA_FILEPATH_DP;node[2]=(UINT8)node_bytes;node[3]=(UINT8)(node_bytes>>8);
    memcpy(node+4,WindowsName,sizeof(WindowsName));node+=node_bytes;
    node[0]=END_DEVICE_PATH_TYPE;node[1]=END_ENTIRE_DEVICE_PATH_SUBTYPE;node[2]=4;node[3]=0;
    s=b->LoadImage(FALSE,Self,(VOID *)path,bytes,3086680,&staged);
  }
  GuFree(bytes);GuFree(path);
  if(s==EFI_SUCCESS) {
    EFI_LOADED_IMAGE_PROTOCOL *im=NULL;s=b->HandleProtocol(staged,&LoadedGuid,(VOID **)&im);
    if(s==EFI_SUCCESS&&(!im||im->DeviceHandle!=volume))s=EFI_UNSUPPORTED;
  }
  if(s==EFI_SUCCESS)Windows=staged;
  else if(staged&&b->UnloadImage)(VOID)b->UnloadImage(staged);
  GuLog("Windows image staged from own ESP: status=%#llx SHA256 pinned\n",(unsigned long long)s);
  return s;
}
EFI_STATUS GuBootChoose(BOOLEAN *unlock) {
  if(!unlock||!Table||!Table->ConIn||!Table->ConIn->ReadKeyStroke)return EFI_UNSUPPORTED;
  *unlock=FALSE;
  GuBootSay(L"\r\nRTX 4070 SUPER - experimental manual trial; hardware unvalidated.\r\nU: one unlock attempt, then Windows only if all checks pass.\r\nS/Esc or 30 seconds: stock Windows, no GPU transaction.\r\nThe display may freeze while GPU drivers are released.\r\nApplication console output stops; firmware callbacks remain active.\r\nKeep peripherals unchanged. If Windows does not appear within 4 minutes,\r\npower OFF fully, wait 10 seconds, then choose Windows Boot Manager.\r\nLog: ESP EFI/4070S-PROBE/LOGS/RUNxxxx.TXT. No automatic retry or reset.\r\n");
  for(UINTN i=0;i<300;i++) {
    EFI_INPUT_KEY key;EFI_STATUS s=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);
    if(s==EFI_SUCCESS) {
      if(key.UnicodeChar=='u'||key.UnicodeChar=='U'){*unlock=TRUE;return EFI_SUCCESS;}
      if(key.ScanCode==0x17||key.UnicodeChar==27||key.UnicodeChar=='s'||key.UnicodeChar=='S')return EFI_SUCCESS;
    } else if(s!=EFI_NOT_READY)return s;
    s=Table->BootServices->Stall(100000);if(s!=EFI_SUCCESS)return s;
  }
  return EFI_SUCCESS;
}
EFI_STATUS GuBootClose(VOID) {
  EFI_STATUS s=EFI_SUCCESS;
  if(Log){s=Log->Flush(Log);EFI_STATUS closed=Log->Close(Log);Log=NULL;if(s==EFI_SUCCESS)s=closed;}
  if(Root){EFI_STATUS closed=Root->Close(Root);Root=NULL;if(s==EFI_SUCCESS)s=closed;}
  return s;
}
EFI_STATUS GuBootWindows(VOID) {
  if(!Windows)return EFI_NOT_READY;
  return Table->BootServices->StartImage(Windows,NULL,NULL);
}
VOID GuBootHalt(VOID) {
  GuBootSay(L"\r\nSTOPPED. Power OFF fully; next boot use Windows Boot Manager.\r\n");
  for(;;)__asm__ volatile("pause");
}
