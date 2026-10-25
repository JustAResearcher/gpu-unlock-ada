/* SPDX-License-Identifier: GPL-2.0-only */
/* Adapted 2026-10-24 for gpu-unlock-ada; see ../THIRD_PARTY_NOTICES.md. */
/* Adapted from TOTORONG's UEFI handoff; this port is not hardware validated. */
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
static EFI_GUID FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const CHAR16 WindowsPath[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
static EFI_HANDLE WindowsImage,WindowsVolume;
static EFI_DEVICE_PATH_PROTOCOL *WindowsDevicePath;
static EFI_STATUS MakeWindowsPath(VOID) {
  EFI_DEVICE_PATH_PROTOCOL *base=NULL;
  EFI_STATUS s=Gu.bs->HandleProtocol(WindowsVolume,&PathGuid,(VOID**)&base);
  if(EFI_ERROR(s)||!base)return EFI_NOT_FOUND;
  UINTN off=0;BOOLEAN end=FALSE;
  for(UINTN i=0;i<128 && off<=4092;i++) {
    EFI_DEVICE_PATH_PROTOCOL *n=(VOID*)((UINT8*)base+off);
    UINTN len=n->Length[0]+((UINTN)n->Length[1]<<8);
    if(len<4||len>4096-off)return EFI_COMPROMISED_DATA;
    if(n->Type==END_DEVICE_PATH_TYPE) {
      if(n->SubType!=END_ENTIRE_DEVICE_PATH_SUBTYPE||len!=4)return EFI_UNSUPPORTED;
      end=TRUE;break;
    }
    off+=len;
  }
  if(!end)return EFI_COMPROMISED_DATA;
  UINTN node=4+sizeof(WindowsPath),total=off+node+4;
  UINT8 *b=GuAlloc(total);if(!b)return EFI_OUT_OF_RESOURCES;
  memcpy(b,base,off);b[off]=MEDIA_DEVICE_PATH;b[off+1]=MEDIA_FILEPATH_DP;
  b[off+2]=(UINT8)node;b[off+3]=(UINT8)(node>>8);memcpy(b+off+4,WindowsPath,sizeof(WindowsPath));
  b[off+node]=END_DEVICE_PATH_TYPE;b[off+node+1]=END_ENTIRE_DEVICE_PATH_SUBTYPE;b[off+node+2]=4;
  WindowsDevicePath=(VOID*)b;return EFI_SUCCESS;
}
static EFI_STATUS PreloadWindows(VOID) {
  EFI_HANDLE *h=NULL;UINTN n=0,found=0;
  EFI_STATUS s=Gu.bs->LocateHandleBuffer(ByProtocol,&FsGuid,NULL,&n,&h);
  if(EFI_ERROR(s))return s;
  if(!h||n>256){GuFree(h);return EFI_COMPROMISED_DATA;}
  for(UINTN i=0;i<n;i++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;EFI_FILE_PROTOCOL *root=NULL,*file=NULL;
    EFI_STATUS x=Gu.bs->HandleProtocol(h[i],&FsGuid,(VOID**)&fs);
    if(!EFI_ERROR(x)&&fs)x=fs->OpenVolume(fs,&root);
    if(EFI_ERROR(x)||!root){s=EFI_DEVICE_ERROR;break;}
    x=root->Open(root,&file,(CHAR16*)WindowsPath,EFI_FILE_MODE_READ,0);
    if(!EFI_ERROR(x)&&file) {
      UINT8 mz[2];UINTN bytes=2;EFI_STATUS y=file->Read(file,&bytes,mz);
      EFI_STATUS closed=file->Close(file);
      if(EFI_ERROR(y)||EFI_ERROR(closed)||bytes!=2||mz[0]!='M'||mz[1]!='Z')s=EFI_COMPROMISED_DATA;
      else {found++;WindowsVolume=h[i];}
    } else if(x!=EFI_NOT_FOUND)s=EFI_DEVICE_ERROR;
    x=root->Close(root);if(EFI_ERROR(x))s=x;
    if(EFI_ERROR(s))break;
  }
  GuFree(h);GuLog("Windows candidates=%zu status=%#llx\n",(size_t)found,(unsigned long long)s);
  if(EFI_ERROR(s))return s;
  if(found!=1)return found?EFI_NO_MAPPING:EFI_NOT_FOUND;
  s=MakeWindowsPath();if(EFI_ERROR(s))return s;
  s=Gu.bs->LoadImage(FALSE,Gu.image,WindowsDevicePath,NULL,0,&WindowsImage);
  GuLog("Windows LoadImage=%#llx\n",(unsigned long long)s);
  if(EFI_ERROR(s))return s;
  EFI_LOADED_IMAGE_PROTOCOL *li=NULL;
  s=Gu.bs->HandleProtocol(WindowsImage,&LoadedGuid,(VOID**)&li);
  if(EFI_ERROR(s)||!li||li->DeviceHandle!=WindowsVolume||li->ImageCodeType!=EfiLoaderCode)return EFI_COMPROMISED_DATA;
  return GuLogReady()?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
static EFI_STATUS VerifySelectors(const char *phase) {
  UINT32 ss0=GuRead32(0x82381c),ss1=GuRead32(0x823820);
  GuLog("%s SS0=0x%08x SS1=0x%08x\n",phase,ss0,ss1);
  GuLog("%s unlock_matched=%u; handoff checks GPU accessibility separately\n",phase,ss0==0x88888888 && ss1==8);
  UINT32 readout=GuRead32(0x823818),plm=GuRead32(0x823804);
  GuLog("%s effective=%#x FEAT=%#x\n",phase,readout,plm);
  return !Gu.fault && GuLogReady() && ss0==0x88888888 && ss1==8 &&
    !(readout&0x07ffffff) && plm==0xffffffff?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
static EFI_STATUS RestoreDisplayForWindows(EFI_HANDLE Driver) {
  if(!WindowsImage || !Driver || !GuScreenDetached || !GuSafeToReturn() || !Gu.quiesced ||
     Gu.flrs!=1 || Gu.allocations || Gu.dma_count || !Gu.owned || Gu.fault)return EFI_NOT_READY;
  EFI_STATUS s=VerifySelectors("POST-FLR");if(EFI_ERROR(s))return s;
  GuLogText("stage=before_GOP_reconnect; transaction quiesced, DMA released, PCI restored\n");
  if(!GuLogReady())return EFI_DEVICE_ERROR;
  s=Gu.bs->CloseProtocol(Gu.handle,&PciGuid,Gu.image,NULL);if(EFI_ERROR(s))return s;
  Gu.owned=FALSE;
  EFI_HANDLE drivers[2]={Driver,NULL};
  s=Gu.bs->ConnectController(Gu.handle,drivers,NULL,TRUE);
  GuLog("ConnectController status=%#llx\n",(unsigned long long)s);
  if(EFI_ERROR(s))return s;
  s=GuRefreshTargetPci();if(EFI_ERROR(s))return s;
  ExpectDetached=FALSE;
  if(!DisplayCheck())return EFI_DEVICE_ERROR;
  UINT16 command=0;UINT32 value=0;
  s=GuConfigRead(EfiPciIoWidthUint16,4,&command);
  if(EFI_ERROR(s)||command!=(UINT16)InitialConfig[1]||(command&4))return EFI_DEVICE_ERROR;
  for(UINT32 offset=0x10;offset<=0x24;offset+=4) {
    s=GuConfigRead(EfiPciIoWidthUint32,offset,&value);
    if(EFI_ERROR(s)||value!=InitialConfig[offset/4])return EFI_DEVICE_ERROR;
  }
  s=VerifySelectors("POST-GOP");if(EFI_ERROR(s))return s;
  GuLogText("stage=before_StartImage; POST-GOP selectors and PCI BAR/command verified\n");
  if(!GuLogReady())return EFI_DEVICE_ERROR;
  GuScreenDetached=FALSE;
  return EFI_SUCCESS;
}
