/* SPDX-License-Identifier: GPL-2.0-only */
/* Fake FAT/EFI only: no GPU, firmware variable, disk or boot writes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../src/session.c"

static EFI_FILE_PROTOCOL root,parent,file;
static EFI_LOADED_IMAGE_PROTOCOL loaded;
static struct {HARDDRIVE_DEVICE_PATH hd;EFI_DEVICE_PATH_PROTOCOL end;} volume;
static UINT8 image_path[256],options[GU_SESSION_BYTES],disk[GU_SESSION_BYTES];
static UINTN position,file_size;
static UINT64 attributes;
static unsigned opens,writes,flushes,closes;
static BOOLEAN fail_write,short_write,fail_flush,fail_close,short_read,stale_readback;

static EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *This) {
  assert(This==&file);closes++;
  if(stale_readback && closes==1)memcpy(disk,"GUARM001",8);
  return fail_close?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *This,EFI_GUID *Kind,UINTN *Size,VOID *Data) {
  (void)Kind;assert(This==&file && *Size>=sizeof(EFI_FILE_INFO));
  EFI_FILE_INFO *info=Data;memset(Data,0,sizeof(*info));
  info->Size=sizeof(*info);info->FileSize=file_size;info->Attribute=attributes;*Size=sizeof(*info);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *This,UINTN *Size,VOID *Data) {
  assert(This==&file && *Size==GU_SESSION_BYTES && position==0);
  if(short_read)(*Size)--;memcpy(Data,disk,*Size);position+=*Size;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Position(EFI_FILE_PROTOCOL *This,UINT64 Offset) {
  assert(This==&file && Offset==0);position=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write(EFI_FILE_PROTOCOL *This,UINTN *Size,VOID *Data) {
  assert(This==&file && *Size==GU_SESSION_BYTES && position==0);writes++;
  if(fail_write)return EFI_DEVICE_ERROR;
  if(short_write)*Size=4;memcpy(disk,Data,*Size);position+=*Size;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Flush(EFI_FILE_PROTOCOL *This) {
  assert(This==&file);flushes++;return fail_flush?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *This,EFI_FILE_PROTOCOL **Out,
 CHAR16 *Name,UINT64 Mode,UINT64 Attributes) {
  assert(!Attributes);
  if(This==&root) {
    CHAR16 expected[64];const CHAR16 prefix[]=L"\\EFI\\GpuUnlockSession\\";
    UINTN n=0;for(;prefix[n];n++)expected[n]=prefix[n];
    const UINT8 *actual=loaded.LoadOptions;
    for(UINTN i=0;i<32;i++)expected[n++]=(CHAR16)actual[8+i];expected[n]=0;
    assert(!memcmp(Name,expected,(n+1)*sizeof(CHAR16)));
    assert(Mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE));*Out=&parent;
  } else {
    assert(This==&parent);const CHAR16 expected[]=L"REQUEST.BIN";
    assert(!memcmp(Name,expected,sizeof(expected)));
    assert(Mode==(opens?EFI_FILE_MODE_READ:EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE));
    opens++;position=0;*Out=&file;
  }
  return EFI_SUCCESS;
}
static VOID Reset(VOID) {
  memset(&root,0,sizeof(root));memset(&parent,0,sizeof(parent));memset(&file,0,sizeof(file));
  memset(&loaded,0,sizeof(loaded));memset(&volume,0,sizeof(volume));memset(image_path,0,sizeof(image_path));
  root.Open=parent.Open=Open;file.Close=Close;file.GetInfo=Info;file.Read=Read;
  file.SetPosition=Position;file.Write=Write;file.Flush=Flush;
  memcpy(options,"GUSSN001",8);memset(options+8,'a',32);memcpy(options+40,GU_PROFILE_TAG,64);
  memcpy(disk,options,sizeof(disk));memcpy(disk,"GUARM001",8);
  loaded.LoadOptions=options;loaded.LoadOptionsSize=sizeof(options);loaded.FilePath=(VOID *)image_path;
  const CHAR16 path[]=L"\\EFI\\GpuUnlockSession\\aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\BOOTX64.EFI";
  UINTN size=4+sizeof(path);image_path[0]=MEDIA_DEVICE_PATH;image_path[1]=MEDIA_FILEPATH_DP;
  image_path[2]=(UINT8)size;image_path[3]=(UINT8)(size>>8);memcpy(image_path+4,path,sizeof(path));
  image_path[size]=END_DEVICE_PATH_TYPE;image_path[size+1]=END_ENTIRE_DEVICE_PATH_SUBTYPE;image_path[size+2]=4;
  volume.hd.Header.Type=MEDIA_DEVICE_PATH;volume.hd.Header.SubType=MEDIA_HARDDRIVE_DP;
  volume.hd.Header.Length[0]=sizeof(volume.hd);volume.hd.PartitionNumber=1;
  volume.hd.PartitionStart=2048;volume.hd.PartitionSize=524288;
  volume.hd.Signature[0]=1;volume.hd.SignatureType=SIGNATURE_TYPE_GUID;
  volume.hd.MBRType=MBR_TYPE_EFI_PARTITION_TABLE_HEADER;
  volume.end.Type=END_DEVICE_PATH_TYPE;volume.end.SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;volume.end.Length[0]=4;
  file_size=sizeof(disk);position=attributes=0;opens=writes=flushes=closes=0;
  fail_write=short_write=fail_flush=fail_close=short_read=stale_readback=FALSE;
  Ready=Tried=FALSE;
}
static EFI_STATUS Attach(VOID) {
  EFI_FILE_PROTOCOL *out=NULL;
  EFI_STATUS status=GuSessionOpenParent(&loaded,(VOID *)&volume,&root,&out);
  if(!EFI_ERROR(status))assert(out==&parent);else assert(!out);
  return status;
}
static UINT16 U16(const UINT8 *p) {return (UINT16)(p[0]|((UINT16)p[1]<<8));}
static UINTN ReadFixture(const char *Path,UINT8 *Bytes,UINTN Maximum) {
  FILE *input=fopen(Path,"rb");assert(input);
  UINTN n=fread(Bytes,1,Maximum,input);assert(!ferror(input));assert(fgetc(input)==EOF);
  assert(!fclose(input));return n;
}
static VOID PowerShellFixture(const char *OptionPath,const char *RequestPath) {
  Reset();
  UINT8 boot[4096];UINTN bytes=ReadFixture(OptionPath,boot,sizeof(boot));
  assert(bytes>6 && boot[0]==1 && !boot[1] && !boot[2] && !boot[3]);
  UINTN path_bytes=U16(boot+4),start=6;
  while(start+2<=bytes && U16(boot+start))start+=2;
  assert(start+2<=bytes);start+=2;
  assert(path_bytes>=50 && path_bytes<=bytes-start && bytes-start-path_bytes==GU_SESSION_BYTES);
  /* Emulate firmware resolving the actual HD node to a partition DeviceHandle.
     Its LoadedImage.FilePath is the remaining FilePath + End portion. */
  assert(boot[start]==MEDIA_DEVICE_PATH && boot[start+1]==MEDIA_HARDDRIVE_DP);
  assert(U16(boot+start+2)==sizeof(volume.hd));memcpy(&volume.hd,boot+start,sizeof(volume.hd));
  loaded.FilePath=(VOID *)(boot+start+sizeof(volume.hd));
  loaded.LoadOptions=boot+start+path_bytes;loaded.LoadOptionsSize=GU_SESSION_BYTES;
  assert(ReadFixture(RequestPath,disk,sizeof(disk))==GU_SESSION_BYTES);
  assert(!EFI_ERROR(Attach()));assert(!EFI_ERROR(GuSessionConsume(&parent)));
  assert(!memcmp(disk,"GUSED001",8) && !memcmp(disk+8,(UINT8 *)loaded.LoadOptions+8,96));
  assert(GuSessionConsume(&parent)==EFI_ACCESS_DENIED && writes==1);
  /* The production backend's packet is not interchangeable with another permit. */
  opens=0;memcpy(disk,"GUARM001",8);disk[8]=disk[8]=='a'?'b':'a';
  assert(!EFI_ERROR(Attach()));assert(EFI_ERROR(GuSessionConsume(&parent)) && writes==1);
  puts("PowerShell staging bytes accepted by actual C helper; replay/mismatched permit refused");
}
int main(int argc,char **argv) {
  if(argc==3){PowerShellFixture(argv[1],argv[2]);return 0;}
  assert(argc==1);
  Reset();assert(!EFI_ERROR(Attach()));assert(!EFI_ERROR(GuSessionConsume(&parent)));
  assert(opens==2 && writes==1 && flushes==1 && closes==2 && !memcmp(disk,"GUSED001",8));
  assert(GuSessionConsume(&parent)==EFI_ACCESS_DENIED && writes==1);
  /* Even a fresh invocation with the same options cannot reuse the consumed permit. */
  opens=0;assert(!EFI_ERROR(Attach()));assert(EFI_ERROR(GuSessionConsume(&parent)));assert(writes==1);
  Reset();assert(GuSessionConsume(&parent)==EFI_ACCESS_DENIED && !writes);
  Reset();loaded.LoadOptionsSize--;assert(EFI_ERROR(Attach()));
  Reset();loaded.LoadOptions=NULL;assert(EFI_ERROR(Attach()));
  Reset();options[0]='X';assert(EFI_ERROR(Attach()));
  Reset();options[8]='A';assert(EFI_ERROR(Attach()));
  Reset();memset(options+8,'0',32);assert(EFI_ERROR(Attach()));
  Reset();options[40]=options[40]=='a'?'b':'a';assert(EFI_ERROR(Attach()));
  Reset();image_path[4]='x';assert(EFI_ERROR(Attach()));
  Reset();image_path[2]-=2;assert(EFI_ERROR(Attach()));
  Reset();volume.hd.SignatureType=1;assert(EFI_ERROR(Attach()));
  Reset();volume.hd.MBRType=1;assert(EFI_ERROR(Attach()));
  Reset();volume.hd.PartitionNumber=0;assert(EFI_ERROR(Attach()));
  Reset();volume.hd.Signature[0]=0;assert(EFI_ERROR(Attach()));
  Reset();volume.end.SubType=1;assert(EFI_ERROR(Attach()));
  Reset();assert(!EFI_ERROR(Attach()));disk[8]='b';assert(EFI_ERROR(GuSessionConsume(&parent)) && !writes);
  Reset();assert(!EFI_ERROR(Attach()));disk[40]='x';assert(EFI_ERROR(GuSessionConsume(&parent)) && !writes);
  Reset();assert(!EFI_ERROR(Attach()));memcpy(disk,"GUSED001",8);assert(EFI_ERROR(GuSessionConsume(&parent)) && !writes);
  Reset();assert(!EFI_ERROR(Attach()));file_size--;assert(EFI_ERROR(GuSessionConsume(&parent)) && !writes);
  Reset();assert(!EFI_ERROR(Attach()));file_size++;assert(EFI_ERROR(GuSessionConsume(&parent)) && !writes);
  Reset();assert(!EFI_ERROR(Attach()));attributes=EFI_FILE_DIRECTORY;assert(EFI_ERROR(GuSessionConsume(&parent)) && !writes);
  Reset();assert(!EFI_ERROR(Attach()));attributes=EFI_FILE_READ_ONLY;assert(EFI_ERROR(GuSessionConsume(&parent)) && !writes);
  Reset();assert(!EFI_ERROR(Attach()));short_read=TRUE;assert(EFI_ERROR(GuSessionConsume(&parent)) && !writes);
  Reset();assert(!EFI_ERROR(Attach()));fail_write=TRUE;assert(EFI_ERROR(GuSessionConsume(&parent)) && !flushes);
  assert(GuSessionConsume(&parent)==EFI_ACCESS_DENIED && writes==1);
  Reset();assert(!EFI_ERROR(Attach()));short_write=TRUE;assert(EFI_ERROR(GuSessionConsume(&parent)) && !flushes);
  Reset();assert(!EFI_ERROR(Attach()));fail_flush=TRUE;assert(EFI_ERROR(GuSessionConsume(&parent)) && opens==1);
  Reset();assert(!EFI_ERROR(Attach()));fail_close=TRUE;assert(EFI_ERROR(GuSessionConsume(&parent)) && opens==1);
  Reset();assert(!EFI_ERROR(Attach()));stale_readback=TRUE;assert(EFI_ERROR(GuSessionConsume(&parent)) && opens==2);
  puts("session permit: valid path, profile binding, one-shot replay and IO failure tests passed");
  return 0;
}
