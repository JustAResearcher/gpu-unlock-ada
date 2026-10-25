/* SPDX-License-Identifier: GPL-2.0-only */
/* Adapted 2026-10-24 for gpu-unlock-ada; see ../THIRD_PARTY_NOTICES.md. */
#include "app_io.h"
#include "compat/compat.h"
#include "format.h"
#include "sha256.h"
#include "session.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
static EFI_GUID LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID,
  FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,InfoGuid=EFI_FILE_INFO_ID;
static EFI_SYSTEM_TABLE *Table;
static EFI_FILE_PROTOCOL *Root,*Parent,*Run,*Report;
static BOOLEAN LogFault;
#if !GU_ENABLE_UNLOCK
#define CAPTURE_BUFFER_BYTES 8192U
#define CAPTURE_LIMIT_BYTES (2U*1024U*1024U)
static EFI_FILE_PROTOCOL *Capture;
static UINT8 CaptureBuffer[CAPTURE_BUFFER_BYTES];
static UINTN CaptureUsed,CaptureBytes;
static EFI_STATUS CaptureFault;
#endif
BOOLEAN GuScreenDetached;
#if GU_ENABLE_UNLOCK
typedef struct {const char *name;CHAR16 *file;UINTN size;const char *hash;} ASSET_SPEC;
static const ASSET_SPEC Specs[]={
 {"gpu-unlock/610.43.03/booter_load_ad102.bin",L"ASSETS\\booter_load_ad102.bin",57672,"6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49"},
 {"gpu-unlock/610.43.03/v67-ad102-610.43.03-four-plm.bin",L"ASSETS\\v67-ad102-610.43.03-four-plm.bin",64000,"44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb"},
 {"gpu-unlock/610.43.03/gsp_ga10x.bin",L"ASSETS\\gsp_ga10x.bin",84277400,"572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a"},
 {"gpu-unlock/610.43.03/gsp_boot_ad102.bin",L"ASSETS\\gsp_boot_ad102.bin",36864,"45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0"},
 {"gpu-unlock/610.43.03/gsp_boot_desc_ad102.bin",L"ASSETS\\gsp_boot_desc_ad102.bin",84,"83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42"}
};
#endif
static BOOLEAN IoFailed(EFI_STATUS Status){return Status!=EFI_SUCCESS;}
static VOID LogError(VOID){LogFault=TRUE;if(Gu.armed)Gu.fault=TRUE;}
VOID GuConsole(const char *Text) {
  if(GuScreenDetached)return;
  if(!Table || !Table->ConOut || !Table->ConOut->OutputString){LogError();return;}
  while(*Text) {
    CHAR16 line[128];UINTN n=0;
    while(*Text && n<126){if(*Text=='\n')line[n++]='\r';line[n++]=(UINT8)*Text++;}
    line[n]=0;if(IoFailed(Table->ConOut->OutputString(Table->ConOut,line))){LogError();return;}
  }
}
VOID GuLogText(const char *Text) {
  GuConsole(Text);
  if(!Report || LogFault){LogError();return;}
  UINTN expected=strlen(Text),written=expected;
  EFI_STATUS status=Report->Write(Report,&written,(VOID *)Text);
  if(IoFailed(status) || written!=expected || IoFailed(Report->Flush(Report)))LogError();
}
VOID GuLog(const char *Format,...) {
  char text[1024];va_list args;va_start(args,Format);
  size_t n=GuFormatV(text,sizeof(text),Format,args);va_end(args);
  if(n>=sizeof(text)){LogError();GuConsole("Log format or length error.\n");return;}
  GuLogText(text);
}
BOOLEAN GuLogReady(VOID){return Report && !LogFault;}
static BOOLEAN __attribute__((unused)) UsbPath(EFI_HANDLE Handle) {
  EFI_DEVICE_PATH_PROTOCOL *path=NULL;
  if(IoFailed(Table->BootServices->HandleProtocol(Handle,&PathGuid,(VOID **)&path)) || !path)return FALSE;
  UINTN total=0;BOOLEAN usb=FALSE;
  for(UINTN nodes=0;nodes<64 && total+4<=4096;nodes++) {
    UINTN n=path->Length[0]+((UINTN)path->Length[1]<<8);
    if(n<4 || n>4096-total)return FALSE;
    if(path->Type==MESSAGING_DEVICE_PATH &&
      ((path->SubType==MSG_USB_DP && n==sizeof(USB_DEVICE_PATH)) ||
       (path->SubType==MSG_SD_DP && n==sizeof(SD_DEVICE_PATH))))usb=TRUE;
    if(path->Type==END_DEVICE_PATH_TYPE)return usb && n==4 && path->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE;
    total+=n;path=(VOID *)((UINT8 *)path+n);
  }
  return FALSE;
}
static EFI_STATUS Metadata(EFI_FILE_PROTOCOL *File,BOOLEAN Directory,UINTN Expected) {
  union {EFI_FILE_INFO info;UINT8 bytes[512];} data;UINTN n=sizeof(data);
  EFI_STATUS status=File->GetInfo(File,&InfoGuid,&n,&data);
  if(IoFailed(status))return status;
  if(n<SIZE_OF_EFI_FILE_INFO || n>sizeof(data) || data.info.Size>n || data.info.Size<SIZE_OF_EFI_FILE_INFO ||
    !!(data.info.Attribute&EFI_FILE_DIRECTORY)!=!!Directory ||
    (!Directory && data.info.FileSize!=Expected))return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}
#if !GU_ENABLE_UNLOCK
/* Capture errors are independent of the ordinary report. In particular, they
 * must not silence the summary explaining why a metadata capture is partial. */
static EFI_STATUS CaptureFail(EFI_STATUS Status) {
  if(!IoFailed(CaptureFault))CaptureFault=Status;
  if(Capture) {
    EFI_FILE_PROTOCOL *file=Capture;Capture=NULL;
    EFI_STATUS closed=file->Close(file);
    if(!IoFailed(CaptureFault) && IoFailed(closed))CaptureFault=closed;
  }
  CaptureUsed=0;
  return CaptureFault;
}
static EFI_STATUS CaptureDrain(VOID) {
  if(IoFailed(CaptureFault))return CaptureFault;
  if(!Capture)return EFI_NOT_READY;
  if(CaptureUsed) {
    UINTN size=CaptureUsed;
    EFI_STATUS status=Capture->Write(Capture,&size,CaptureBuffer);
    if(IoFailed(status) || size!=CaptureUsed)
      return CaptureFail(IoFailed(status)?status:EFI_DEVICE_ERROR);
    CaptureUsed=0;
  }
  return EFI_SUCCESS;
}
#endif
EFI_STATUS GuCaptureOpen(VOID) {
#if GU_ENABLE_UNLOCK
  return EFI_ACCESS_DENIED;
#else
  if(Capture)return CaptureFail(EFI_ALREADY_STARTED);
  CaptureFault=EFI_SUCCESS;CaptureUsed=CaptureBytes=0;
  if(!Run || !GuLogReady())return CaptureFail(EFI_NOT_READY);
  EFI_FILE_PROTOCOL *existing=NULL;
  EFI_STATUS status=Run->Open(Run,&existing,L"TOPOLOGY.TXT",EFI_FILE_MODE_READ,0);
  if(!IoFailed(status)) {
    if(!existing)return CaptureFail(EFI_DEVICE_ERROR);
    status=existing->Close(existing);
    return CaptureFail(IoFailed(status)?status:EFI_ALREADY_STARTED);
  }
  if(existing) {
    /* A failed Open returning a handle is not an acceptable creation result. */
    existing->Close(existing);return CaptureFail(EFI_DEVICE_ERROR);
  }
  if(status!=EFI_NOT_FOUND)return CaptureFail(status);
  status=Run->Open(Run,&Capture,L"TOPOLOGY.TXT",
    EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
  if(IoFailed(status) || !Capture)return CaptureFail(IoFailed(status)?status:EFI_DEVICE_ERROR);
  /* UEFI has no exclusive-create flag. This run directory was created by us;
   * also reject a preexisting nonempty file if one appeared after the probe. */
  status=Metadata(Capture,FALSE,0);
  if(IoFailed(status))return CaptureFail(status);
  return EFI_SUCCESS;
#endif
}
EFI_STATUS GuCaptureWrite(const char *Text) {
#if GU_ENABLE_UNLOCK
  return EFI_ACCESS_DENIED;
#else
  if(IoFailed(CaptureFault))return CaptureFault;
  if(!Capture)return EFI_NOT_READY;
  if(!Text)return CaptureFail(EFI_INVALID_PARAMETER);
  UINTN length=0;
  while(length<=CAPTURE_BUFFER_BYTES && Text[length])length++;
  if(length>CAPTURE_BUFFER_BYTES || length>CAPTURE_LIMIT_BYTES-CaptureBytes)
    return CaptureFail(EFI_BAD_BUFFER_SIZE);
  CaptureBytes+=length;
  for(UINTN offset=0;offset<length;) {
    UINTN size=CAPTURE_BUFFER_BYTES-CaptureUsed;
    if(size>length-offset)size=length-offset;
    memcpy(CaptureBuffer+CaptureUsed,Text+offset,size);
    CaptureUsed+=size;offset+=size;
    if(CaptureUsed==CAPTURE_BUFFER_BYTES) {
      EFI_STATUS status=CaptureDrain();if(IoFailed(status))return status;
    }
  }
  return EFI_SUCCESS;
#endif
}
EFI_STATUS GuCaptureClose(VOID) {
#if GU_ENABLE_UNLOCK
  return EFI_ACCESS_DENIED;
#else
  if(IoFailed(CaptureFault))return CaptureFault;
  if(!Capture)return EFI_NOT_READY;
  EFI_STATUS status=CaptureDrain();
  if(IoFailed(status))return status;
  status=Capture->Flush(Capture);
  if(IoFailed(status))return CaptureFail(status);
  EFI_FILE_PROTOCOL *file=Capture;Capture=NULL;
  status=file->Close(file);
  if(IoFailed(status))return CaptureFail(status);
  return EFI_SUCCESS;
#endif
}
EFI_STATUS GuIoOpen(EFI_HANDLE Image,EFI_SYSTEM_TABLE *St) {
#if !GU_ENABLE_UNLOCK
  if(Capture)return EFI_ALREADY_STARTED;
  CaptureFault=EFI_SUCCESS;CaptureUsed=CaptureBytes=0;
#endif
  Table=St;Root=Parent=Run=Report=NULL;LogFault=FALSE;memset(GuAssets,0,sizeof(GuAssets));
  EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
  EFI_STATUS status=Table->BootServices->HandleProtocol(Image,&LoadedGuid,(VOID **)&loaded);
  if(IoFailed(status) || !loaded)return EFI_UNSUPPORTED;
#if !GU_SESSION_HELPER
  if(!UsbPath(loaded->DeviceHandle))return EFI_UNSUPPORTED;
#endif
  status=Table->BootServices->HandleProtocol(loaded->DeviceHandle,&FsGuid,(VOID **)&fs);
  if(IoFailed(status) || !fs)return EFI_UNSUPPORTED;
  status=fs->OpenVolume(fs,&Root);if(IoFailed(status) || !Root)return EFI_DEVICE_ERROR;
#if GU_SESSION_HELPER
  EFI_DEVICE_PATH_PROTOCOL *volume_path=NULL;
  status=Table->BootServices->HandleProtocol(loaded->DeviceHandle,&PathGuid,(VOID **)&volume_path);
  if(IoFailed(status))return status;
  status=GuSessionOpenParent(loaded,volume_path,Root,&Parent);
#else
  status=Root->Open(Root,&Parent,L"\\GPU-UNLOCK-ADA",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
#endif
  if(IoFailed(status) || !Parent)return EFI_NOT_FOUND;
  status=Metadata(Parent,TRUE,0);if(IoFailed(status))return status;
  for(UINTN slot=0;slot<100;slot++) {
    CHAR16 name[]=L"RUN00";name[3]=(CHAR16)('0'+slot/10);name[4]=(CHAR16)('0'+slot%10);
    EFI_FILE_PROTOCOL *existing=NULL;
    status=Parent->Open(Parent,&existing,name,EFI_FILE_MODE_READ,0);
    if(!IoFailed(status)) {
      if(!existing || IoFailed(existing->Close(existing)))return EFI_DEVICE_ERROR;continue;
    }
    if(status!=EFI_NOT_FOUND)return status;
    status=Parent->Open(Parent,&Run,name,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
    if(IoFailed(status) || !Run)return EFI_DEVICE_ERROR;
    status=Metadata(Run,TRUE,0);if(IoFailed(status))return status;
    status=Run->Open(Run,&existing,L"REPORT.TXT",EFI_FILE_MODE_READ,0);
    if(!IoFailed(status)){if(existing)existing->Close(existing);return EFI_ALREADY_STARTED;}
    if(status!=EFI_NOT_FOUND)return status;
    status=Run->Open(Run,&Report,L"REPORT.TXT",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
    if(IoFailed(status) || !Report)return EFI_DEVICE_ERROR;
#if GU_STAGED_UNLOCK
    GuLog("gpu-unlock-ada Windows preboot port 0.6.4-unlock-test\nRUN%02u\nPROFILE_TAG=%s mode=staged-unlock-test\n",
      (unsigned)slot,GU_PROFILE_TAG);
#elif GU_REHEARSAL
    GuLog("gpu-unlock-ada Windows preboot port 0.5.2-rehearsal\nRUN%02u\nPROFILE_TAG=%s mode=firmware-rehearsal\n",
      (unsigned)slot,GU_PROFILE_TAG);
#else
    GuLog("gpu-unlock-ada Windows preboot port 0.4.2\nRUN%02u\nPROFILE_TAG=%s mode=%s\n",
      (unsigned)slot,GU_PROFILE_TAG,GU_ENABLE_UNLOCK?"experimental-unlock":"diagnostic");
#endif
    return GuLogReady()?EFI_SUCCESS:EFI_DEVICE_ERROR;
  }
  return EFI_VOLUME_FULL;
}
EFI_STATUS GuIoConsumeSession(VOID) {
#if GU_ENABLE_UNLOCK && GU_SESSION_HELPER
  return GuSessionConsume(Parent);
#else
  return EFI_ACCESS_DENIED;
#endif
}
EFI_STATUS GuLoadAssets(VOID) {
#if !GU_ENABLE_UNLOCK
  return EFI_ACCESS_DENIED;
#else
  for(UINTN i=0;i<5;i++) {
    EFI_FILE_PROTOCOL *file=NULL;const ASSET_SPEC *spec=&Specs[i];
    EFI_STATUS status=Parent->Open(Parent,&file,spec->file,EFI_FILE_MODE_READ,0);
    if(IoFailed(status) || !file) {
      /* Some firmware returns a handle along with a failed/warning open.
       * Close that handle without converting the unsuccessful open to success. */
      if(file)file->Close(file);
      return IoFailed(status)?status:EFI_DEVICE_ERROR;
    }
    status=Metadata(file,FALSE,spec->size);
    UINT8 *data=NULL;
    if(!IoFailed(status)) {
      data=GuAlloc(spec->size);if(!data)status=EFI_OUT_OF_RESOURCES;
    }
    for(UINTN done=0;!IoFailed(status) && done<spec->size;) {
      UINTN n=spec->size-done;if(n>1048576)n=1048576;UINTN want=n;
      status=file->Read(file,&n,data+done);
      if(!IoFailed(status) && n!=want)status=EFI_END_OF_FILE;done+=n;
    }
    if(!IoFailed(status)) {
      UINT8 extra=0;UINTN n=1;status=file->Read(file,&n,&extra);
      if(!IoFailed(status)&&n)status=EFI_COMPROMISED_DATA;
      if(!IoFailed(status))status=Metadata(file,FALSE,spec->size);
    }
    EFI_STATUS closed=file->Close(file);if(!IoFailed(status))status=closed;
    if(!IoFailed(status) && !GuHashMatches(data,spec->size,spec->hash))status=EFI_CRC_ERROR;
    if(IoFailed(status)){GuFree(data);return status;}
    GuAssets[i].name=spec->name;GuAssets[i].firmware.data=data;GuAssets[i].firmware.size=spec->size;
    GuLog("asset verified: %s bytes=%zu sha256=%s\n",spec->name,(size_t)spec->size,spec->hash);
    if(!GuLogReady())return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
#endif
}
VOID GuReleaseAssets(VOID) {
  if(!GuSafeToReturn())return;
  for(UINTN i=0;i<5;i++){GuFree((VOID *)GuAssets[i].firmware.data);memset(&GuAssets[i],0,sizeof(GuAssets[i]));}
}
EFI_STATUS GuIoClose(VOID) {
  EFI_FILE_PROTOCOL **files[]={&Report,&Run,&Parent,&Root};EFI_STATUS result=EFI_SUCCESS;
  if(!GuSafeToReturn())return EFI_ACCESS_DENIED;
#if !GU_ENABLE_UNLOCK
  if(Capture)result=GuCaptureClose();
#endif
  for(UINTN i=0;i<4;i++)if(*files[i]) {
    /* EFI_FILE_PROTOCOL.Close invalidates the handle even on error. */
    EFI_FILE_PROTOCOL *file=*files[i];*files[i]=NULL;
    EFI_STATUS status=file->Close(file);
    if(IoFailed(status) && !IoFailed(result))result=status;
  }
  return result;
}
