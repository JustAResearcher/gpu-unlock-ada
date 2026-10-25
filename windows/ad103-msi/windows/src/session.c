/* SPDX-License-Identifier: GPL-2.0-only */
#include "session.h"
#include "compat/compat.h"
#include <Guid/FileInfo.h>

#if GU_SESSION_HELPER
static EFI_GUID InfoGuid=EFI_FILE_INFO_ID;
static UINT8 Permit[GU_SESSION_BYTES];
static BOOLEAN Ready,Tried;

static BOOLEAN LowerHex(const UINT8 *Text,UINTN Count) {
  BOOLEAN nonzero=FALSE;
  for(UINTN i=0;i<Count;i++) {
    UINT8 c=Text[i];
    if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return FALSE;
    if(c!='0')nonzero=TRUE;
  }
  return nonzero;
}
static BOOLEAN GptVolume(EFI_DEVICE_PATH_PROTOCOL *Path) {
  if(!Path)return FALSE;
  UINTN total=0;BOOLEAN partition=FALSE;
  for(UINTN nodes=0;nodes<64 && total+4<=4096;nodes++) {
    UINTN size=Path->Length[0]+((UINTN)Path->Length[1]<<8);
    if(size<4 || size>4096-total)return FALSE;
    if(Path->Type==END_DEVICE_PATH_TYPE)
      return partition && size==4 && Path->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE;
    if(partition)return FALSE; /* A partition handle must end at its HD node. */
    if(Path->Type==MEDIA_DEVICE_PATH) {
      if(Path->SubType!=MEDIA_HARDDRIVE_DP || size!=sizeof(HARDDRIVE_DEVICE_PATH))return FALSE;
      HARDDRIVE_DEVICE_PATH disk;memcpy(&disk,Path,sizeof(disk));
      BOOLEAN signature=FALSE;
      for(UINTN i=0;i<sizeof(disk.Signature);i++)if(disk.Signature[i])signature=TRUE;
      if(disk.MBRType!=MBR_TYPE_EFI_PARTITION_TABLE_HEADER ||
         disk.SignatureType!=SIGNATURE_TYPE_GUID || !disk.PartitionNumber ||
         !disk.PartitionStart || !disk.PartitionSize || !signature)return FALSE;
      partition=TRUE;
    }
    total+=size;Path=(VOID *)((UINT8 *)Path+size);
  }
  return FALSE;
}
static BOOLEAN ExactImagePath(EFI_DEVICE_PATH_PROTOCOL *Path,const CHAR16 *Expected,UINTN Chars) {
  if(!Path || Path->Type!=MEDIA_DEVICE_PATH || Path->SubType!=MEDIA_FILEPATH_DP)return FALSE;
  UINTN size=Path->Length[0]+((UINTN)Path->Length[1]<<8);
  if(size!=4+(Chars+1)*sizeof(CHAR16))return FALSE;
  if(memcmp((UINT8 *)Path+4,Expected,(Chars+1)*sizeof(CHAR16)))return FALSE;
  EFI_DEVICE_PATH_PROTOCOL *end=(VOID *)((UINT8 *)Path+size);
  return end->Type==END_DEVICE_PATH_TYPE && end->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE &&
    end->Length[0]==4 && end->Length[1]==0;
}
EFI_STATUS GuSessionOpenParent(EFI_LOADED_IMAGE_PROTOCOL *Loaded,
 EFI_DEVICE_PATH_PROTOCOL *VolumePath,EFI_FILE_PROTOCOL *Root,EFI_FILE_PROTOCOL **Parent) {
  Ready=Tried=FALSE;memset(Permit,0,sizeof(Permit));
  if(!Parent)return EFI_INVALID_PARAMETER;*Parent=NULL;
  if(!Loaded || !Root || !Root->Open || !Loaded->LoadOptions ||
     Loaded->LoadOptionsSize!=GU_SESSION_BYTES || !GptVolume(VolumePath))return EFI_ACCESS_DENIED;
  const UINT8 *options=Loaded->LoadOptions;
  if(memcmp(options,"GUSSN001",8) || !LowerHex(options+8,32) ||
     !LowerHex(options+40,64) || strlen(GU_PROFILE_TAG)!=64 ||
     memcmp(options+40,GU_PROFILE_TAG,64))return EFI_SECURITY_VIOLATION;
  CHAR16 directory[64],image[80];const CHAR16 prefix[]=L"\\EFI\\GpuUnlockSession\\";
  UINTN n=0;for(;prefix[n];n++)directory[n]=prefix[n];
  for(UINTN i=0;i<32;i++)directory[n++]=(CHAR16)options[8+i];
  directory[n]=0;memcpy(image,directory,n*sizeof(CHAR16));
  const CHAR16 suffix[]=L"\\BOOTX64.EFI";UINTN length=n;
  for(UINTN i=0;suffix[i];i++)image[length++]=suffix[i];image[length]=0;
  if(!ExactImagePath(Loaded->FilePath,image,length))return EFI_SECURITY_VIOLATION;
  EFI_STATUS status=Root->Open(Root,Parent,directory,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
  if(EFI_ERROR(status) || !*Parent)return EFI_ERROR(status)?status:EFI_DEVICE_ERROR;
  memcpy(Permit,"GUARM001",8);memcpy(Permit+8,options+8,GU_SESSION_BYTES-8);Ready=TRUE;
  return EFI_SUCCESS;
}
static EFI_STATUS ReadPermit(EFI_FILE_PROTOCOL *File,UINT8 *Data) {
  if(!File || !File->GetInfo || !File->Read)return EFI_UNSUPPORTED;
  union {EFI_FILE_INFO info;UINT8 bytes[512];} metadata;UINTN size=sizeof(metadata);
  EFI_STATUS status=File->GetInfo(File,&InfoGuid,&size,&metadata);
  if(EFI_ERROR(status))return status;
  if(size<SIZE_OF_EFI_FILE_INFO || size>sizeof(metadata) || metadata.info.Size>size ||
     metadata.info.Size<SIZE_OF_EFI_FILE_INFO || metadata.info.FileSize!=GU_SESSION_BYTES ||
     (metadata.info.Attribute&(EFI_FILE_DIRECTORY|EFI_FILE_READ_ONLY)))return EFI_COMPROMISED_DATA;
  size=GU_SESSION_BYTES;status=File->Read(File,&size,Data);
  return EFI_ERROR(status)?status:size!=GU_SESSION_BYTES?EFI_END_OF_FILE:EFI_SUCCESS;
}
EFI_STATUS GuSessionConsume(EFI_FILE_PROTOCOL *Parent) {
  if(!Ready || Tried || !Parent || !Parent->Open)return EFI_ACCESS_DENIED;
  Tried=TRUE; /* A partial/failed IO sequence may never retry in this invocation. */
  EFI_FILE_PROTOCOL *file=NULL;
  EFI_STATUS status=Parent->Open(Parent,&file,L"REQUEST.BIN",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
  if(EFI_ERROR(status) || !file)return EFI_ERROR(status)?status:EFI_DEVICE_ERROR;
  if(!file->Close)return EFI_UNSUPPORTED;
  UINT8 observed[GU_SESSION_BYTES];status=ReadPermit(file,observed);
  if(!EFI_ERROR(status) && memcmp(observed,Permit,sizeof(Permit)))status=EFI_SECURITY_VIOLATION;
  if(!EFI_ERROR(status) && (!file->SetPosition || !file->Write || !file->Flush))status=EFI_UNSUPPORTED;
  UINT8 consumed[GU_SESSION_BYTES];memcpy(consumed,Permit,sizeof(consumed));memcpy(consumed,"GUSED001",8);
  if(!EFI_ERROR(status))status=file->SetPosition(file,0);
  if(!EFI_ERROR(status)) {
    UINTN size=sizeof(consumed);status=file->Write(file,&size,consumed);
    if(!EFI_ERROR(status) && size!=sizeof(consumed))status=EFI_DEVICE_ERROR;
  }
  if(!EFI_ERROR(status))status=file->Flush(file);
  EFI_STATUS closed=file->Close(file);file=NULL;if(!EFI_ERROR(status))status=closed;
  if(EFI_ERROR(status))return status;
  /* Close/reopen and compare after a successful Flush. An IO acknowledgement
     cannot promise storage persistence through physical power loss. */
  status=Parent->Open(Parent,&file,L"REQUEST.BIN",EFI_FILE_MODE_READ,0);
  if(EFI_ERROR(status) || !file)return EFI_ERROR(status)?status:EFI_DEVICE_ERROR;
  if(!file->Close)return EFI_UNSUPPORTED;
  status=ReadPermit(file,observed);
  if(!EFI_ERROR(status) && memcmp(observed,consumed,sizeof(consumed)))status=EFI_SECURITY_VIOLATION;
  closed=file->Close(file);if(!EFI_ERROR(status))status=closed;
  Ready=FALSE;return status;
}
#else
EFI_STATUS GuSessionOpenParent(EFI_LOADED_IMAGE_PROTOCOL *Loaded,
 EFI_DEVICE_PATH_PROTOCOL *VolumePath,EFI_FILE_PROTOCOL *Root,EFI_FILE_PROTOCOL **Parent) {
  (void)Loaded;(void)VolumePath;(void)Root;if(Parent)*Parent=NULL;return EFI_UNSUPPORTED;
}
EFI_STATUS GuSessionConsume(EFI_FILE_PROTOCOL *Parent) {(void)Parent;return EFI_UNSUPPORTED;}
#endif
