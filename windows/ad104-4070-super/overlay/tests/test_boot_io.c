/* SPDX-License-Identifier: GPL-2.0-only */
#include "boot4070s_config.h"
#include "../boot_io.c"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <setjmp.h>
GU_PLATFORM Gu;GU_ASSET GuAssets[5];
#include "boot4070s_fixtures.h"
enum SUITE {SOURCE,ASSETS,WIN,CHOICE,LOGGING,INTEGRATION};
enum MUTATION {NONE,WRONG_GUID,NUMBER,START,SECTORS,NOT_GPT,USB,USB_CLASS,USB_WWID,MALFORMED,MULTI,TWO_HD,NO_HD,DUPLICATE,DUPLICATE_GEOMETRY,NO_FS,NO_DEVICE,
 MISSING,SIZE,SHORT,TAIL,HASH_FIRST,HASH_LAST,READ_ERROR,FILE_CLOSE,ROOT_CLOSE,LOADED_DEVICE,LOAD_ERROR,ENUM_ERROR,ENUM_MISSING,OTHER_ESP,
 EXISTING_CLOSE,PROBE_ERROR,DIR_CLOSE,WRITE_ERROR,SHORT_WRITE,FLUSH_ERROR,LOG_CLOSE,KEY_U,KEY_S,KEY_ESC,TIMEOUT,KEY_ERROR,STALL_ERROR,SILENCE};
struct TEST {const char *name;enum SUITE suite;enum MUTATION mutation;unsigned file;};
#define T(n,s,m,f) {n,s,m,f}
#define A(n,m) T("asset-booter-" n,ASSETS,m,0),T("asset-v67-" n,ASSETS,m,1),T("asset-gsp-" n,ASSETS,m,2),T("asset-boot-" n,ASSETS,m,3),T("asset-desc-" n,ASSETS,m,4)
static const struct TEST Tests[]={
 T("source-exact-internal-esp",SOURCE,NONE,0),
 T("source-wrong-guid",SOURCE,WRONG_GUID,0),T("source-wrong-number",SOURCE,NUMBER,0),T("source-wrong-start",SOURCE,START,0),T("source-wrong-sectors",SOURCE,SECTORS,0),
 T("source-not-gpt",SOURCE,NOT_GPT,0),T("source-usb",SOURCE,USB,0),T("source-usb-class",SOURCE,USB_CLASS,0),T("source-malformed",SOURCE,MALFORMED,0),
 T("source-usb-wwid",SOURCE,USB_WWID,0),
 T("source-multi-instance",SOURCE,MULTI,0),T("source-two-hd",SOURCE,TWO_HD,0),T("source-no-hd",SOURCE,NO_HD,0),T("source-duplicate",SOURCE,DUPLICATE,0),
 T("source-duplicate-guid-wrong-geometry",SOURCE,DUPLICATE_GEOMETRY,0),T("source-no-filesystem",SOURCE,NO_FS,0),T("source-no-loaded-device",SOURCE,NO_DEVICE,0),
 T("assets-exact-native-c",ASSETS,NONE,0),T("assets-no-native-filesystem",ASSETS,NO_FS,2),T("assets-wrong-guid",ASSETS,WRONG_GUID,2),T("assets-wrong-number",ASSETS,NUMBER,2),
 T("assets-wrong-start",ASSETS,START,2),T("assets-wrong-sectors",ASSETS,SECTORS,2),T("assets-not-gpt",ASSETS,NOT_GPT,2),T("assets-usb",ASSETS,USB,2),
 T("assets-usb-class",ASSETS,USB_CLASS,2),T("assets-usb-wwid",ASSETS,USB_WWID,2),
 T("assets-malformed",ASSETS,MALFORMED,2),T("assets-multi-instance",ASSETS,MULTI,2),T("assets-duplicate",ASSETS,DUPLICATE,2),
 T("assets-duplicate-guid-wrong-geometry",ASSETS,DUPLICATE_GEOMETRY,2),T("assets-native-root-close",ASSETS,ROOT_CLOSE,2),
 A("missing",MISSING),A("wrong-size",SIZE),A("short-read",SHORT),A("tail",TAIL),A("hash-first",HASH_FIRST),A("hash-last",HASH_LAST),A("read-error",READ_ERROR),A("close-error",FILE_CLOSE),
 T("windows-current-image-same-esp",WIN,NONE,5),T("windows-duplicate-esp",WIN,DUPLICATE,5),T("windows-duplicate-guid-wrong-geometry",WIN,DUPLICATE_GEOMETRY,5),
 T("windows-source-guid-changed",WIN,WRONG_GUID,5),T("windows-only-other-esp",WIN,OTHER_ESP,5),T("windows-usb",WIN,USB,5),T("windows-malformed",WIN,MALFORMED,5),
 T("windows-usb-class",WIN,USB_CLASS,5),T("windows-usb-wwid",WIN,USB_WWID,5),
 T("windows-enumeration-error",WIN,ENUM_ERROR,5),T("windows-own-esp-not-enumerated",WIN,ENUM_MISSING,5),
 T("windows-missing",WIN,MISSING,5),T("windows-wrong-size",WIN,SIZE,5),T("windows-short-read",WIN,SHORT,5),T("windows-tail",WIN,TAIL,5),
 T("windows-hash-first",WIN,HASH_FIRST,5),T("windows-hash-last",WIN,HASH_LAST,5),T("windows-read-error",WIN,READ_ERROR,5),T("windows-file-close",WIN,FILE_CLOSE,5),
 T("windows-root-close",WIN,ROOT_CLOSE,5),T("windows-loaded-device-mismatch",WIN,LOADED_DEVICE,5),T("windows-load-error",WIN,LOAD_ERROR,5),
 T("choice-u",CHOICE,KEY_U,0),T("choice-s",CHOICE,KEY_S,0),T("choice-escape",CHOICE,KEY_ESC,0),T("choice-default-30-seconds",CHOICE,TIMEOUT,0),
 T("choice-key-error",CHOICE,KEY_ERROR,0),T("choice-stall-error",CHOICE,STALL_ERROR,0),T("silence-keeps-firmware-console",CHOICE,SILENCE,0),
 T("log-new-file-no-overwrite",LOGGING,NONE,0),T("log-existing-close-error",LOGGING,EXISTING_CLOSE,0),T("log-probe-error",LOGGING,PROBE_ERROR,0),
 T("log-directory-close-error",LOGGING,DIR_CLOSE,0),T("log-write-error",LOGGING,WRITE_ERROR,0),T("log-short-write",LOGGING,SHORT_WRITE,0),
 T("log-flush-error",LOGGING,FLUSH_ERROR,0),T("log-close-error",LOGGING,LOG_CLOSE,0),T("integration-exact-internal-boot",INTEGRATION,NONE,0)
};
#undef A
#undef T
static const struct TEST *Current;
static BOOLEAN Injected;
static unsigned Checks,Failures,Loads,Starts,FileWrites,IllegalWrites,UnexpectedReads,RawRequests,ControllerChanges;
static unsigned LogCreates,LogCloses,ExistingCloses,DirOpens,DirCloses,Stalls,Keys,ConsoleCalls;
static CHAR16 Console[4096],ExistingName[80],MissingName[80];
static jmp_buf Escape;
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"SEMANTIC_FAIL case=%s line=%d %s\n",Current->name,__LINE__,#x);longjmp(Escape,1);}}while(0)
#include "boot4070s_profile_traps.h"
enum {OWN,NATIVE_C,SECOND,VOLUMES};
struct VOLUME {EFI_SIMPLE_FILE_SYSTEM_PROTOCOL fs;EFI_FILE_PROTOCOL root;UINT8 path[4096];UINTN path_bytes;BOOLEAN available;unsigned opens,closes;};
struct READER {EFI_FILE_PROTOCOL file;UINTN position;unsigned opens,closes,eof;};
static struct VOLUME Volumes[VOLUMES];static struct READER Readers[BOOT_FILE_COUNT];
static EFI_BOOT_SERVICES Bs;static EFI_SYSTEM_TABLE St;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Input;static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Output;
static EFI_LOADED_IMAGE_PROTOCOL SelfImage,WinImage;static EFI_FILE_PROTOCOL Dir,LogFile,Existing;
static const EFI_GUID OwnGuid=GU_WINDOWS_PARTITION_INITIALIZER,AssetGuid=GU_ASSET_PARTITION_INITIALIZER;
static const CHAR16 LogDirectory[]=L"\\EFI\\4070S-PROBE\\LOGS";
/* Independent contract oracle: never derive expected bytes from boot_io.c. */
static const CHAR16 ExpectedWindowsName[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
static const CHAR16 *const SmallPaths[5]={
 L"\\EFI\\4070S-PROBE\\ASSETS\\booter_load_ad102.bin",L"\\EFI\\4070S-PROBE\\ASSETS\\v67-ad102-610.43.03-four-plm.bin",GU_GSP_PATH,
 L"\\EFI\\4070S-PROBE\\ASSETS\\gsp_boot_ad102.bin",L"\\EFI\\4070S-PROBE\\ASSETS\\gsp_boot_desc_ad102.bin"};
static VOID *Allocations[32];static unsigned AllocCount;
VOID *GuAlloc(size_t n){CHECK(AllocCount<32);VOID *p=calloc(1,n);CHECK(p);Allocations[AllocCount++]=p;return p;}
VOID GuFree(VOID *p){if(!p)return;for(unsigned i=0;i<AllocCount;i++)if(Allocations[i]==p){Allocations[i]=Allocations[--AllocCount];free(p);return;}CHECK(0);}
static EFI_HANDLE VolumeHandle(unsigned i){return (EFI_HANDLE)(UINTN)(i+2);}
static unsigned ReaderIndex(EFI_FILE_PROTOCOL *p){for(unsigned i=0;i<BOOT_FILE_COUNT;i++)if(p==&Readers[i].file)return i;CHECK(0);return 0;}
static BOOLEAN FileMutation(unsigned i,enum MUTATION m){return Current->file==i&&Current->mutation==m;}
static void CopyName(CHAR16 *out,const CHAR16 *s){CHECK(wcslen(s)<80);wcscpy(out,s);}
static EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *p){
 for(unsigned i=0;i<VOLUMES;i++)if(p==&Volumes[i].root){Volumes[i].closes++;CHECK(Volumes[i].closes<=Volumes[i].opens);
  if(Current->mutation==ROOT_CLOSE&&i==(Current->suite==ASSETS?NATIVE_C:OWN)){Injected=TRUE;return EFI_DEVICE_ERROR;}return EFI_SUCCESS;}
 if(p==&Dir){DirCloses++;return Current->mutation==DIR_CLOSE?EFI_DEVICE_ERROR:EFI_SUCCESS;}
 if(p==&Existing){ExistingCloses++;return Current->mutation==EXISTING_CLOSE?EFI_DEVICE_ERROR:EFI_SUCCESS;}
 if(p==&LogFile){LogCloses++;return Current->mutation==LOG_CLOSE?EFI_DEVICE_ERROR:EFI_SUCCESS;}
 unsigned i=ReaderIndex(p);Readers[i].closes++;CHECK(Readers[i].closes<=Readers[i].opens);
 if(FileMutation(i,FILE_CLOSE)){Injected=TRUE;return EFI_DEVICE_ERROR;}return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Flush(EFI_FILE_PROTOCOL *p){CHECK(p==&LogFile);return Current->mutation==FLUSH_ERROR?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Delete(EFI_FILE_PROTOCOL *p){(void)p;IllegalWrites++;return EFI_WRITE_PROTECTED;}
static EFI_STATUS EFIAPI SetInfo(EFI_FILE_PROTOCOL *p,EFI_GUID *g,UINTN n,VOID *data){(void)p;(void)g;(void)n;(void)data;IllegalWrites++;return EFI_WRITE_PROTECTED;}
static EFI_STATUS EFIAPI Connect(EFI_HANDLE h,EFI_HANDLE *drivers,EFI_DEVICE_PATH_PROTOCOL *path,BOOLEAN recursive){(void)h;(void)drivers;(void)path;(void)recursive;ControllerChanges++;return EFI_UNSUPPORTED;}
static EFI_STATUS EFIAPI Disconnect(EFI_HANDLE h,EFI_HANDLE driver,EFI_HANDLE child){(void)h;(void)driver;(void)child;ControllerChanges++;return EFI_UNSUPPORTED;}
static EFI_STATUS EFIAPI Write(EFI_FILE_PROTOCOL *p,UINTN *n,VOID *data){
 if(p!=&LogFile){IllegalWrites++;return EFI_WRITE_PROTECTED;}CHECK(*n&&data);FileWrites++;
 if(Current->mutation==SHORT_WRITE)(*n)--;return Current->mutation==WRITE_ERROR?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *p,EFI_GUID *g,UINTN *n,VOID *data){
 unsigned i=ReaderIndex(p);CHECK(!memcmp(g,&InfoGuid,16)&&*n>=sizeof(EFI_FILE_INFO));memset(data,0,sizeof(EFI_FILE_INFO));
 if(FileMutation(i,SIZE))Injected=TRUE;
 ((EFI_FILE_INFO *)data)->FileSize=BootSizes[i]+(FileMutation(i,SIZE)?1:0);*n=sizeof(EFI_FILE_INFO);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *p,UINTN *n,VOID *out){
 unsigned i=ReaderIndex(p);struct READER *r=&Readers[i];CHECK(*n&&*n<=1048576&&r->position<=BootSizes[i]);
 UINTN at=r->position,count=BootSizes[i]-at;if(count>*n)count=*n;
 if(!count){r->eof++;if(FileMutation(i,TAIL)){Injected=TRUE;*(UINT8 *)out=0x5a;*n=1;}else *n=0;return EFI_SUCCESS;}
 if(FileMutation(i,READ_ERROR)&&at+count==BootSizes[i]){Injected=TRUE;*n=0;return EFI_DEVICE_ERROR;}
 if(FileMutation(i,SHORT)){Injected=TRUE;count--;}memcpy(out,BootBytes[i]+at,count);
 if(count&&FileMutation(i,HASH_FIRST)&&!at){Injected=TRUE;((UINT8 *)out)[0]^=1;}
 if(count&&FileMutation(i,HASH_LAST)&&at+count==BootSizes[i]){Injected=TRUE;((UINT8 *)out)[count-1]^=1;}
 r->position+=count;*n=count;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *p,EFI_FILE_PROTOCOL **out,CHAR16 *name,UINT64 mode,UINT64 attrs){
 *out=NULL;
 if(p==&Volumes[OWN].root&&!wcscmp(name,LogDirectory)){
  CHECK(mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE)&&attrs==EFI_FILE_DIRECTORY);DirOpens++;*out=&Dir;return EFI_SUCCESS;}
 if(p==&Dir){
  CHECK(!attrs&&!wcschr(name,L'\\')&&!wcschr(name,L'/'));
  if(mode==EFI_FILE_MODE_READ){
   if(Current->mutation==PROBE_ERROR)return EFI_DEVICE_ERROR;
   if(!ExistingName[0])CopyName(ExistingName,name);
   if(!wcscmp(ExistingName,name)){*out=&Existing;return EFI_SUCCESS;}CopyName(MissingName,name);return EFI_NOT_FOUND;
  }
  CHECK(mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE));
  CHECK(MissingName[0]&&!wcscmp(name,MissingName)&&wcscmp(name,ExistingName));CHECK(!LogCreates);LogCreates++;*out=&LogFile;return EFI_SUCCESS;
 }
 if(mode!=EFI_FILE_MODE_READ||attrs){IllegalWrites++;return EFI_WRITE_PROTECTED;}
 unsigned i;for(i=0;i<5;i++)if(!wcscmp(name,SmallPaths[i]))break;
 if(!wcscmp(name,ExpectedWindowsName))i=BOOT_WINDOWS;
 /* i==5 from a failed asset-name search must not alias the Windows reader. */
 if(i==BOOT_WINDOWS&&wcscmp(name,ExpectedWindowsName))i=BOOT_FILE_COUNT;
 if(i>=BOOT_FILE_COUNT||p!=&Volumes[i==BOOT_GSP?NATIVE_C:OWN].root){UnexpectedReads++;return EFI_NOT_FOUND;}
 if(FileMutation(i,MISSING)){Injected=TRUE;return EFI_NOT_FOUND;}CHECK(Readers[i].opens==Readers[i].closes);
 Readers[i].position=0;Readers[i].opens++;*out=&Readers[i].file;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs,EFI_FILE_PROTOCOL **out){
 for(unsigned i=0;i<VOLUMES;i++)if(fs==&Volumes[i].fs){CHECK(Volumes[i].available);Volumes[i].opens++;*out=&Volumes[i].root;return EFI_SUCCESS;}
 CHECK(0);return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *g,VOID **out){
 *out=NULL;
 if(!memcmp(g,&LoadedGuid,16)){if(h==(EFI_HANDLE)1)*out=&SelfImage;else if(h==(EFI_HANDLE)10)*out=&WinImage;}
 else if(!memcmp(g,&FsGuid,16)||!memcmp(g,&PathGuid,16)){
  for(unsigned i=0;i<VOLUMES;i++)if(h==VolumeHandle(i)){if(!memcmp(g,&PathGuid,16))*out=Volumes[i].path;else if(Volumes[i].available)*out=&Volumes[i].fs;}
 }else RawRequests++;return *out?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Free(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *g,VOID *key,UINTN *n,EFI_HANDLE **out){
 CHECK(type==ByProtocol&&!key);*n=0;*out=NULL;
 if(memcmp(g,&FsGuid,16)){RawRequests++;return EFI_UNSUPPORTED;}if(Current->mutation==ENUM_ERROR)return EFI_DEVICE_ERROR;
 *out=malloc(VOLUMES*sizeof(**out));CHECK(*out);
 for(unsigned i=0;i<VOLUMES;i++)if(Volumes[i].available&&!(i==OWN&&Current->mutation==ENUM_MISSING))(*out)[(*n)++]=VolumeHandle(i);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Load(BOOLEAN policy,EFI_HANDLE parent,EFI_DEVICE_PATH_PROTOCOL *path,VOID *bytes,UINTN n,EFI_HANDLE *out){
 Loads++;CHECK(!policy&&parent==(EFI_HANDLE)1&&n==BootSizes[BOOT_WINDOWS]);CHECK(bytes&&!memcmp(bytes,BootBytes[BOOT_WINDOWS],n));
 UINTN base=Volumes[OWN].path_bytes-4;CHECK(!memcmp(path,Volumes[OWN].path,base));UINT8 *node=(UINT8 *)path+base;UINTN length=node[2]+((UINTN)node[3]<<8);
 CHECK(node[0]==MEDIA_DEVICE_PATH&&node[1]==MEDIA_FILEPATH_DP&&length==4+sizeof(ExpectedWindowsName));CHECK(!memcmp(node+4,ExpectedWindowsName,sizeof(ExpectedWindowsName)));
 node+=length;CHECK(node[0]==END_DEVICE_PATH_TYPE&&node[1]==END_ENTIRE_DEVICE_PATH_SUBTYPE&&node[2]==4&&!node[3]);
 if(Current->mutation==LOAD_ERROR)return EFI_LOAD_ERROR;*out=(EFI_HANDLE)10;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI StartImage(EFI_HANDLE image,UINTN *n,CHAR16 **data){CHECK(image==(EFI_HANDLE)10&&!n&&!data);Starts++;return EFI_ABORTED;}
static EFI_STATUS EFIAPI Watchdog(UINTN s,UINT64 c,UINTN n,CHAR16 *d){CHECK(!s&&!c&&!n&&!d);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *p,EFI_INPUT_KEY *key){
 CHECK(p==&Input);Keys++;key->ScanCode=0;key->UnicodeChar=0;if(Current->mutation==KEY_ERROR)return EFI_DEVICE_ERROR;
 if(Current->mutation==KEY_U||Current->suite==INTEGRATION){key->UnicodeChar=L'U';return EFI_SUCCESS;}
 if(Current->mutation==KEY_S){key->UnicodeChar=L'S';return EFI_SUCCESS;}if(Current->mutation==KEY_ESC){key->ScanCode=0x17;return EFI_SUCCESS;}return EFI_NOT_READY;
}
static EFI_STATUS EFIAPI Stall(UINTN n){CHECK(n==100000);Stalls++;return Current->mutation==STALL_ERROR?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Say(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *p,CHAR16 *s){CHECK(p==&Output);ConsoleCalls++;CHECK(wcslen(Console)+wcslen(s)<4096);wcscat(Console,s);return EFI_SUCCESS;}
static void MakePath(struct VOLUME *v,BOOLEAN native){
 memset(v->path,0,sizeof(v->path));v->path[0]=1;v->path[1]=1;v->path[2]=6;HARDDRIVE_DEVICE_PATH *hd=(VOID *)(v->path+6);
 hd->Header.Type=4;hd->Header.SubType=1;hd->Header.Length[0]=42;hd->PartitionNumber=native?3:1;
 hd->PartitionStart=native?239616:2048;hd->PartitionSize=native?3998527488ULL:204800;hd->MBRType=hd->SignatureType=2;memcpy(hd->Signature,native?&AssetGuid:&OwnGuid,16);
 v->path[48]=127;v->path[49]=255;v->path[50]=4;v->path_bytes=52;
}
static void MutatePath(unsigned target){
 struct VOLUME *v=&Volumes[target];HARDDRIVE_DEVICE_PATH *hd=(VOID *)(v->path+6);
 switch(Current->mutation){
 case WRONG_GUID:hd->Signature[0]^=1;break;case NUMBER:hd->PartitionNumber++;break;case START:hd->PartitionStart++;break;case SECTORS:hd->PartitionSize++;break;
 case NOT_GPT:hd->MBRType=hd->SignatureType=1;break;case USB:v->path[0]=3;v->path[1]=5;break;
 case USB_CLASS:memmove(v->path+11,v->path+6,46);memset(v->path,0,11);v->path[0]=3;v->path[1]=15;v->path[2]=11;v->path_bytes=57;break;
 case USB_WWID:{
  /* DevicePath.h:466-492: ten-byte WWID prefix plus UTF-16 serial.
   * Move the original matching GPT node/end intact behind this valid node. */
  const CHAR16 serial[]=L"T1";UINTN length=sizeof(USB_WWID_DEVICE_PATH)+sizeof(serial);
  memmove(v->path+length,v->path+6,46);memset(v->path,0,length);
  USB_WWID_DEVICE_PATH *wwid=(VOID *)v->path;
  wwid->Header.Type=3;wwid->Header.SubType=0x10;wwid->Header.Length[0]=(UINT8)length;
  wwid->InterfaceNumber=1;wwid->VendorId=0x1234;wwid->ProductId=0x5678;
  memcpy(v->path+sizeof(*wwid),serial,sizeof(serial));v->path_bytes=length+46;break;
 }
 case MALFORMED:v->path[2]=3;break;case MULTI:v->path[49]=1;break;
 case TWO_HD:memmove(v->path+90,v->path+48,4);memcpy(v->path+48,v->path+6,42);v->path_bytes=94;break;
 case NO_HD:memcpy(v->path+6,v->path+48,4);v->path_bytes=10;break;
 case DUPLICATE:case DUPLICATE_GEOMETRY:case OTHER_ESP:
  memcpy(Volumes[SECOND].path,v->path,sizeof(v->path));Volumes[SECOND].path_bytes=v->path_bytes;Volumes[SECOND].available=TRUE;
  if(Current->mutation==DUPLICATE_GEOMETRY)((HARDDRIVE_DEVICE_PATH *)(VOID *)(Volumes[SECOND].path+6))->PartitionStart++;
  if(Current->mutation==OTHER_ESP)hd->Signature[0]^=1;break;
 case NO_FS:v->available=FALSE;break;default:break;
 }
}
static void Setup(void){
 Table=NULL;Self=Windows=OwnVolume=NULL;Root=Log=NULL;Silent=FALSE;Injected=FALSE;
 Loads=Starts=FileWrites=IllegalWrites=UnexpectedReads=RawRequests=ControllerChanges=0;LogCreates=LogCloses=ExistingCloses=DirOpens=DirCloses=Stalls=Keys=ConsoleCalls=0;
 Console[0]=ExistingName[0]=MissingName[0]=0;
 memset(&Gu,0,sizeof(Gu));memset(GuAssets,0,sizeof(GuAssets));memset(&Bs,0,sizeof(Bs));memset(&St,0,sizeof(St));
 memset(&SelfImage,0,sizeof(SelfImage));memset(&WinImage,0,sizeof(WinImage));memset(Volumes,0,sizeof(Volumes));memset(Readers,0,sizeof(Readers));
 memset(&Dir,0,sizeof(Dir));memset(&LogFile,0,sizeof(LogFile));memset(&Existing,0,sizeof(Existing));
 Bs.HandleProtocol=Handle;Bs.LocateHandleBuffer=Locate;Bs.FreePool=Free;Bs.LoadImage=Load;Bs.StartImage=StartImage;Bs.SetWatchdogTimer=Watchdog;Bs.Stall=Stall;
 Bs.ConnectController=Connect;Bs.DisconnectController=Disconnect;
 memset(&Input,0,sizeof(Input));memset(&Output,0,sizeof(Output));Input.ReadKeyStroke=Key;Output.OutputString=Say;St.BootServices=&Bs;St.ConIn=&Input;St.ConOut=&Output;
 SelfImage.DeviceHandle=Current->mutation==NO_DEVICE?NULL:VolumeHandle(OWN);WinImage.DeviceHandle=Current->mutation==LOADED_DEVICE?VolumeHandle(NATIVE_C):VolumeHandle(OWN);
 for(unsigned i=0;i<VOLUMES;i++){Volumes[i].fs.OpenVolume=Volume;Volumes[i].root.Open=Open;Volumes[i].root.Close=Close;Volumes[i].root.Write=Write;Volumes[i].root.SetInfo=SetInfo;Volumes[i].root.Delete=Delete;Volumes[i].available=i!=SECOND;MakePath(&Volumes[i],i==NATIVE_C);}
 for(unsigned i=0;i<BOOT_FILE_COUNT;i++){Readers[i].file.Read=Read;Readers[i].file.GetInfo=Info;Readers[i].file.Close=Close;Readers[i].file.Write=Write;Readers[i].file.SetInfo=SetInfo;Readers[i].file.Delete=Delete;}
 Dir.Open=Open;Dir.Close=Existing.Close=LogFile.Close=Close;LogFile.Write=Write;LogFile.Flush=Flush;MutatePath(Current->suite==ASSETS?NATIVE_C:OWN);
}
/* Isolate public stages from GuBootOpen's independent RED using its existing
 * session preconditions. All asset/Windows file and hash code remains real. */
static void StageSession(void){Table=&St;Self=(EFI_HANDLE)1;OwnVolume=VolumeHandle(OWN);Root=&Volumes[OWN].root;Log=&LogFile;Volumes[OWN].opens=1;LogCreates=1;}
static void Closed(EFI_STATUS status){
 EFI_STATUS s=GuBootClose();BOOLEAN bad=Current->mutation==LOG_CLOSE||Current->mutation==FLUSH_ERROR||(Current->mutation==ROOT_CLOSE&&Current->suite==WIN);
 CHECK(bad?s!=EFI_SUCCESS:s==EFI_SUCCESS);
 for(unsigned i=0;i<VOLUMES;i++)CHECK(Volumes[i].opens==Volumes[i].closes);
 for(unsigned i=0;i<BOOT_FILE_COUNT;i++)CHECK(Readers[i].opens==Readers[i].closes);
 CHECK(LogCreates==LogCloses&&DirOpens==DirCloses);CHECK(!IllegalWrites&&!RawRequests&&!UnexpectedReads&&!ControllerChanges);CHECK(!Starts||status==EFI_SUCCESS);
}
static void ExpectStatus(EFI_STATUS s,BOOLEAN accepted,const char *stage){
 if((s==EFI_SUCCESS)!=accepted)fprintf(stderr,"STATUS case=%s stage=%s observed=%#llx expected_%s\n",Current->name,stage,(unsigned long long)s,accepted?"success":"refusal");
 CHECK((s==EFI_SUCCESS)==accepted);
}
static void Run(void){
 EFI_STATUS s=EFI_SUCCESS;
 if(Current->suite==SOURCE||Current->suite==LOGGING||Current->suite==INTEGRATION){
  s=GuBootOpen((EFI_HANDLE)1,&St);BOOLEAN refuse=Current->suite==SOURCE&&Current->mutation!=NONE;
  refuse=refuse||Current->mutation==EXISTING_CLOSE||Current->mutation==PROBE_ERROR||Current->mutation==DIR_CLOSE;
  ExpectStatus(s,!refuse,"open");
  if(refuse){CHECK(!Loads&&!Starts&&!FileWrites);Closed(s);return;}CHECK(LogCreates==1&&ExistingCloses==1&&DirCloses==1);
 }else StageSession();
 if(Current->suite==ASSETS||Current->suite==INTEGRATION){
  s=GuBootLoadAssets();ExpectStatus(s,Current->mutation==NONE,"assets");CHECK(!Loads&&!Starts);
  if(Current->mutation>=MISSING&&Current->mutation<=ROOT_CLOSE)CHECK(Injected);
  if(s==EFI_SUCCESS){CHECK(GuAd104AssetsValid());
   for(unsigned i=0;i<5;i++){CHECK(Readers[i].opens==1&&Readers[i].closes==1&&Readers[i].eof==1);CHECK(GuAssets[i].firmware.size==BootSizes[i]&&!memcmp(GuAssets[i].firmware.data,BootBytes[i],BootSizes[i]));}
   CHECK(Volumes[NATIVE_C].opens==1&&Volumes[NATIVE_C].closes==1);
  }
 }
 if(Current->suite==WIN||Current->suite==INTEGRATION){
  s=GuBootLoadWindows();ExpectStatus(s,Current->mutation==NONE,"windows");
  if(Current->mutation>=MISSING&&Current->mutation<=ROOT_CLOSE)CHECK(Injected);
  if(s==EFI_SUCCESS){CHECK(Loads==1&&Readers[BOOT_WINDOWS].opens==1&&Readers[BOOT_WINDOWS].closes==1&&Readers[BOOT_WINDOWS].eof==1);if(Current->suite==WIN)CHECK(GuBootWindows()==EFI_ABORTED&&Starts==1);}
  else CHECK(!Starts&&Loads==((Current->mutation==LOADED_DEVICE||Current->mutation==LOAD_ERROR)?1u:0u));
 }
 if(Current->suite==CHOICE||Current->suite==INTEGRATION){
  if(Current->mutation==SILENCE){EFI_TEXT_STRING output=Output.OutputString;GuBootSay(L"before\n");CHECK(ConsoleCalls==1);GuBootSilence();GuBootSay(L"hidden\n");CHECK(ConsoleCalls==1&&Output.OutputString==output);CHECK(Output.OutputString(&Output,L"firmware remains usable\n")==EFI_SUCCESS&&ConsoleCalls==2);}
  else {BOOLEAN unlock=TRUE;s=GuBootChoose(&unlock);
   if(Current->mutation==KEY_ERROR||Current->mutation==STALL_ERROR)CHECK(s!=EFI_SUCCESS&&!unlock);
   else {CHECK(s==EFI_SUCCESS&&unlock==(Current->mutation==KEY_U||Current->suite==INTEGRATION));CHECK(wcsstr(Console,L"4070 SUPER")&&wcsstr(Console,L"Windows")&&wcsstr(Console,L"30"));CHECK(wcsstr(Console,L"U")&&wcsstr(Console,L"S"));
    if(Current->mutation==TIMEOUT)CHECK(Stalls==300&&Keys==300);else CHECK(!Stalls&&Keys==1);}
  }
 }
 if(Current->suite==LOGGING){GuLog("bounded log write %u\n",7u);CHECK(FileWrites==1);CHECK(Gu.fault==(Current->mutation==WRITE_ERROR||Current->mutation==SHORT_WRITE||Current->mutation==FLUSH_ERROR));}
 if(Current->suite==INTEGRATION)CHECK(GuBootWindows()==EFI_ABORTED&&Starts==1);
 Closed(s);
}
int main(int argc,char **argv){
 BootLoadFixtures(argc,argv);volatile unsigned ran=0;
 for(volatile unsigned i=0;i<sizeof(Tests)/sizeof(Tests[0]);i++){
  Current=&Tests[i];if(argc==4&&strcmp(argv[3],Current->name))continue;ran++;Setup();
  if(!setjmp(Escape)){Run();printf("PASS %s\n",Current->name);}else Failures++;
  while(AllocCount)free(Allocations[--AllocCount]);
 }
 BootFreeFixtures();if(!ran)BootFixtureError("case-name","no matching test");
 printf("boot4070s I/O: %u cases, %u checks, %u semantic failures; real file bytes, simulated EFI only.\n",ran,Checks,Failures);return Failures?1:0;
}
