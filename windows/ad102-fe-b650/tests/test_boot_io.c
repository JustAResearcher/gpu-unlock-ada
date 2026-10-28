#define GU_HOST_TEST_PROFILE 1
/* SPDX-License-Identifier: GPL-2.0-only */
#include "../boot_io.c"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
enum CASE {GOOD,NON_USB,WRONG_PARTITION,BAD_WINDOWS_HASH,BAD_FILE_SIZE,SHORT_READ,WRONG_LOADED_DEVICE,LOG_WRITE_FAIL,IO_CASES};
static enum CASE Case;static unsigned Checks,Loads,Starts,FileWrites;
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"IO FAIL case=%d line=%d %s\n",Case,__LINE__,#x);exit(1);}}while(0)
GU_PLATFORM Gu;GU_ASSET GuAssets[5];
static EFI_BOOT_SERVICES Bs;static EFI_SYSTEM_TABLE St;
static EFI_LOADED_IMAGE_PROTOCOL SelfImage,WinImage;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL OwnFs,WinFs;
static EFI_FILE_PROTOCOL OwnRoot,WinRoot,Dir,LogFile,Reader;
static UINT8 UsbPath[]={3,5,6,0,0,0,127,255,4,0},WinPath[46];
static UINT8 *WinBytes,*AssetBytes[5],*ReadBytes;static UINTN ReadSize,Position;
static unsigned KeyMode,Stalls;
VOID *GuAlloc(size_t n){return calloc(1,n);}
VOID GuFree(VOID *p){free(p);}
BOOLEAN GuProfileAssetsValid(VOID){for(UINTN i=0;i<5;i++)if(!GuAssets[i].firmware.data)return FALSE;return TRUE;}
static EFI_STATUS EFIAPI Free(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *p){(void)p;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Flush(EFI_FILE_PROTOCOL *p){CHECK(p==&LogFile);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Write(EFI_FILE_PROTOCOL *p,UINTN *n,VOID *bytes){CHECK(p==&LogFile&&*n&&bytes);FileWrites++;return Case==LOG_WRITE_FAIL?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *p,EFI_GUID *g,UINTN *n,VOID *data){
  CHECK(p==&Reader&&!memcmp(g,&InfoGuid,16)&&*n>=sizeof(EFI_FILE_INFO));
  memset(data,0,sizeof(EFI_FILE_INFO));((EFI_FILE_INFO *)data)->FileSize=ReadSize+(Case==BAD_FILE_SIZE?1:0);*n=sizeof(EFI_FILE_INFO);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *p,UINTN *n,VOID *out){
  CHECK(p==&Reader);UINTN count=ReadSize-Position;if(count>*n)count=*n;
  if(Case==SHORT_READ&&count)count--;
  memcpy(out,ReadBytes+Position,count);Position+=count;*n=count;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *p,EFI_FILE_PROTOCOL **out,CHAR16 *name,UINT64 mode,UINT64 attrs){
  *out=NULL;
  if(p==&OwnRoot&&mode&EFI_FILE_MODE_CREATE){CHECK(!wcscmp(name,L"\\AD102-UNLOCK")&&(attrs&EFI_FILE_DIRECTORY));*out=&Dir;return EFI_SUCCESS;}
  if(p==&Dir) {
    if(mode==EFI_FILE_MODE_READ)return EFI_NOT_FOUND;
    CHECK(mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE)&&!attrs);*out=&LogFile;return EFI_SUCCESS;
  }
  CHECK(mode==EFI_FILE_MODE_READ&&!attrs);Position=0;
  if(p==&WinRoot){CHECK(!wcscmp(name,WindowsName));ReadBytes=WinBytes;ReadSize=GU_WINDOWS_BOOTMGR_BYTES;}
  else {
    CHECK(p==&OwnRoot);static const CHAR16 *names[]={L"\\AD102-ASSETS\\booter_load_ad102.bin",L"\\AD102-ASSETS\\v67-ad102-610.43.03-four-plm.bin",L"\\AD102-ASSETS\\gsp_ga10x.bin",L"\\AD102-ASSETS\\gsp_boot_ad102.bin",L"\\AD102-ASSETS\\gsp_boot_desc_ad102.bin"};
    static const UINTN sizes[]={57672,64000,84277400,36864,84};UINTN i;
    for(i=0;i<5;i++)if(!wcscmp(name,names[i]))break;CHECK(i<5);ReadBytes=AssetBytes[i];ReadSize=sizes[i];
  }
  *out=&Reader;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs,EFI_FILE_PROTOCOL **root){CHECK(fs==&OwnFs||fs==&WinFs);*root=fs==&OwnFs?&OwnRoot:&WinRoot;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *g,VOID **out){
  *out=NULL;
  if(!memcmp(g,&LoadedGuid,16)){if(h==(EFI_HANDLE)1)*out=&SelfImage;else if(h==(EFI_HANDLE)10)*out=&WinImage;}
  if(!memcmp(g,&FsGuid,16)){if(h==(EFI_HANDLE)2)*out=&OwnFs;else if(h==(EFI_HANDLE)3)*out=&WinFs;}
  if(!memcmp(g,&PathGuid,16)){if(h==(EFI_HANDLE)2)*out=UsbPath;else if(h==(EFI_HANDLE)3)*out=WinPath;}
  return *out?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *g,VOID *key,UINTN *n,EFI_HANDLE **out){
  CHECK(type==ByProtocol&&!memcmp(g,&FsGuid,16)&&!key);*n=2;*out=malloc(2*sizeof(**out));CHECK(*out);(*out)[0]=(EFI_HANDLE)2;(*out)[1]=(EFI_HANDLE)3;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Load(BOOLEAN policy,EFI_HANDLE parent,EFI_DEVICE_PATH_PROTOCOL *path,VOID *bytes,UINTN n,EFI_HANDLE *out){
  CHECK(!policy&&parent==(EFI_HANDLE)1&&n==GU_WINDOWS_BOOTMGR_BYTES&&!memcmp(bytes,WinBytes,n));Loads++;
  CHECK(!memcmp(path,WinPath,42));UINT8 *p=(UINT8 *)path+42;
  CHECK(p[0]==4&&p[1]==4&&!memcmp(p+4,WindowsName,sizeof(WindowsName)));
  *out=(EFI_HANDLE)10;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Start(EFI_HANDLE image,UINTN *n,CHAR16 **data){CHECK(image==(EFI_HANDLE)10&&!n&&!data);Starts++;return EFI_ABORTED;}
static EFI_STATUS EFIAPI Watchdog(UINTN seconds,UINT64 code,UINTN n,CHAR16 *data){CHECK(!seconds&&!code&&!n&&!data);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *p,EFI_INPUT_KEY *key){(void)p;key->ScanCode=0;key->UnicodeChar=KeyMode==1?'U':KeyMode==2?'S':0;return KeyMode?EFI_SUCCESS:EFI_NOT_READY;}
static EFI_STATUS EFIAPI Stall(UINTN n){CHECK(n==100000);Stalls++;return EFI_SUCCESS;}
static void Setup(void) {
  Table=NULL;Self=Windows=OwnVolume=NULL;Root=Log=NULL;Silent=FALSE;Loads=Starts=FileWrites=Stalls=KeyMode=0;
  memset(&Gu,0,sizeof(Gu));memset(GuAssets,0,sizeof(GuAssets));memset(&Bs,0,sizeof(Bs));memset(&St,0,sizeof(St));
  memset(&SelfImage,0,sizeof(SelfImage));memset(&WinImage,0,sizeof(WinImage));
  Bs.HandleProtocol=Handle;Bs.LocateHandleBuffer=Locate;Bs.FreePool=Free;Bs.LoadImage=Load;Bs.StartImage=Start;Bs.SetWatchdogTimer=Watchdog;Bs.Stall=Stall;
  St.BootServices=&Bs;static EFI_SIMPLE_TEXT_INPUT_PROTOCOL input;memset(&input,0,sizeof(input));input.ReadKeyStroke=Key;St.ConIn=&input;
  SelfImage.DeviceHandle=(EFI_HANDLE)2;WinImage.DeviceHandle=(EFI_HANDLE)(UINTN)(Case==WRONG_LOADED_DEVICE?4:3);
  OwnFs.OpenVolume=WinFs.OpenVolume=Volume;
  memset(&OwnRoot,0,sizeof(OwnRoot));memset(&WinRoot,0,sizeof(WinRoot));memset(&Dir,0,sizeof(Dir));memset(&LogFile,0,sizeof(LogFile));memset(&Reader,0,sizeof(Reader));
  OwnRoot.Open=WinRoot.Open=Dir.Open=Open;OwnRoot.Close=WinRoot.Close=Dir.Close=LogFile.Close=Reader.Close=Close;
  LogFile.Flush=Flush;LogFile.Write=Write;Reader.GetInfo=Info;Reader.Read=Read;
  UsbPath[0]=Case==NON_USB?1:3;
  memset(WinPath,0,sizeof(WinPath));HARDDRIVE_DEVICE_PATH *hd=(VOID *)WinPath;
  hd->Header.Type=4;hd->Header.SubType=1;hd->Header.Length[0]=42;hd->PartitionNumber=1;hd->PartitionStart=2048;hd->PartitionSize=204800;
  hd->MBRType=hd->SignatureType=2;memcpy(hd->Signature,&WindowsPartition,16);if(Case==WRONG_PARTITION)hd->Signature[0]^=1;
  WinPath[42]=127;WinPath[43]=255;WinPath[44]=4;
}
static UINT8 *File(const char *path,UINTN expected){FILE *f=fopen(path,"rb");CHECK(f);UINT8 *p=malloc(expected);CHECK(p&&fread(p,1,expected,f)==expected&&fgetc(f)==EOF);fclose(f);return p;}
int main(int argc,char **argv){
  CHECK(argc==3);WinBytes=File(argv[1],GU_WINDOWS_BOOTMGR_BYTES);
  static const char *names[]={"booter_load_ad102.bin","v67-ad102-610.43.03-four-plm.bin","gsp_ga10x.bin","gsp_boot_ad102.bin","gsp_boot_desc_ad102.bin"};
  static const UINTN sizes[]={57672,64000,84277400,36864,84};
  for(UINTN i=0;i<5;i++){char p[4096];snprintf(p,sizeof(p),"%s/%s",argv[2],names[i]);AssetBytes[i]=File(p,sizes[i]);}
  for(Case=GOOD;Case<IO_CASES;Case++) {
    Setup();EFI_STATUS s=GuBootOpen((EFI_HANDLE)1,&St);
    if(Case==NON_USB){CHECK(s!=EFI_SUCCESS&&!FileWrites&&!Loads);continue;}CHECK(s==EFI_SUCCESS);
    if(Case==GOOD){CHECK(GuBootLoadAssets()==EFI_SUCCESS);for(UINTN i=0;i<5;i++){CHECK(GuAssets[i].firmware.size==sizes[i]&&!memcmp(GuAssets[i].firmware.data,AssetBytes[i],sizes[i]));free((VOID *)GuAssets[i].firmware.data);}}
    if(Case==BAD_WINDOWS_HASH)WinBytes[1]^=1;
    s=GuBootLoadWindows();if(Case==BAD_WINDOWS_HASH)WinBytes[1]^=1;
    if(Case==GOOD||Case==LOG_WRITE_FAIL){CHECK(s==EFI_SUCCESS&&Loads==1);if(Case==LOG_WRITE_FAIL)CHECK(Gu.fault);}
    else {CHECK(s!=EFI_SUCCESS&&!Starts);if(Case!=WRONG_LOADED_DEVICE)CHECK(!Loads);}
    if(Case==GOOD) {
      BOOLEAN unlock=FALSE;KeyMode=1;CHECK(GuBootChoose(&unlock)==EFI_SUCCESS&&unlock);
      KeyMode=2;CHECK(GuBootChoose(&unlock)==EFI_SUCCESS&&!unlock);
      KeyMode=0;CHECK(GuBootChoose(&unlock)==EFI_SUCCESS&&!unlock&&Stalls==300);
      CHECK(GuBootWindows()==EFI_ABORTED&&Starts==1);
    }
    CHECK(GuBootClose()==EFI_SUCCESS);printf("boot I/O case %d PASS loads=%u starts=%u\n",Case,Loads,Starts);
  }
  for(UINTN i=0;i<5;i++)free(AssetBytes[i]);free(WinBytes);
  printf("boot I/O: %u checks; %u cases, real file bytes, all EFI services simulated.\n",Checks,IO_CASES);return 0;
}
