/* SPDX-License-Identifier: MIT */
#include "../src/probe.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#if defined(PROBE_TARGET_AD102_FE)
#define HOST_CAPTURE "build-ad102-fe/host-capture.json"
#else
#define HOST_CAPTURE "build/host-capture.json"
#endif

static unsigned Checks;
#define CHECK(x) do {Checks++; if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}} while(0)
static UINT8 TargetPath[]={1,1,6,0,0,1,0x7f,0xff,4,0};
static UINT8 IntelPath[]={1,1,6,0,0,2,0x7f,0xff,4,0};
static UINT8 TargetGopPath[]={1,1,6,0,0,1,2,3,8,0,0,0,0,0,0x7f,0xff,4,0};
static UINT8 IntelGopPath[]={1,1,6,0,0,2,2,3,8,0,0,0,0,0,0x7f,0xff,4,0};
static UINT8 UsbVolumePath[]={3,5,6,0,0,0,0x7f,0xff,4,0};
static EFI_PCI_IO_PROTOCOL MockPci[2];
static EFI_GRAPHICS_OUTPUT_PROTOCOL MockGop[2];
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE GopMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION GopInfo;
static EFI_BOOT_SERVICES Bs;
static EFI_RUNTIME_SERVICES Rs;
static EFI_SYSTEM_TABLE St;
static EFI_LOADED_IMAGE_PROTOCOL Loaded;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
static EFI_FILE_PROTOCOL Root,Directory,Log,Existing;
static UINT8 Config[2][256],Rom[150016];
static UINT32 MockProm[0x100000/4];
static BOOLEAN FailProm;
static EFI_HANDLE Image=(EFI_HANDLE)(UINTN)100,Volume=(EFI_HANDLE)(UINTN)101;
static unsigned Scenario,ConfigReads,BarReads,FileWrites,ExistingOpens,RootCloses,DirCloses,LogCloses;
static char Written[sizeof(Report)];
static BOOLEAN FailedConfig,FailedLocation,FailedLog,ShortWrite,FailedFlush,ExistingFirst,NonUsb;
static UINTN MemoryReads;
static BOOLEAN FailMemory,CorruptResource,ChangeIdentity;

static EFI_STATUS EFIAPI ConfigRead(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 off,UINTN count,VOID *out) {
    CHECK(width==EfiPciIoWidthUint32&&off==0&&(count==16||count==64));ConfigReads++;
    if(FailedConfig && self==&MockPci[0]) return EFI_DEVICE_ERROR;
    memcpy(out,Config[self==&MockPci[0]?0:1],count*4);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MemoryRead(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT8 bar,UINT64 offset,UINTN count,VOID *out) {
    static const UINT32 offsets[]={0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,0x823814,0x823818,0x82381c,0x823820,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0,0x840100};
    UINTN i;BOOLEAN found=FALSE;
    CHECK(self==&MockPci[0]&&width==EfiPciIoWidthUint32&&bar==0);
    if(offset>=0x300000&&offset<0x400000) {
        CHECK((count==1||count==1024)&&offset+count*4<=0x400000&&!(offset&3));
        if(FailProm&&offset==0x301000) return EFI_DEVICE_ERROR;
        memcpy(out,(UINT8 *)MockProm+(offset-0x300000),count*4);return EFI_SUCCESS;
    }
    CHECK(count==1);
    for(i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) if(offset==offsets[i]) found=TRUE;
    CHECK(found);MemoryReads++;
    if(FailMemory&&MemoryReads==2) return EFI_DEVICE_ERROR;
    if(ChangeIdentity&&MemoryReads==1) Config[0][8]^=1;
    *(UINT32 *)out=offset==0?PROBE_CHIP:0;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Location(EFI_PCI_IO_PROTOCOL *self,UINTN *seg,UINTN *bus,UINTN *dev,UINTN *fun) {
    if(FailedLocation) return EFI_DEVICE_ERROR;
    *seg=0;*bus=self==&MockPci[0]?1:0;*dev=self==&MockPci[0]?0:2;*fun=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Bar(EFI_PCI_IO_PROTOCOL *self,UINT8 index,UINT64 *supports,VOID **resources) {
    UINT8 *p;UINT64 min=PROBE_BAR0,max=PROBE_BAR0+0xffffffull,length=0x1000000;
    (void)self;CHECK(index<6);BarReads++;*supports=0;
    p=calloc(1,46);CHECK(p!=NULL);p[0]=0x8a;p[1]=43;p[3]=0;
    memcpy(p+14,&min,8);memcpy(p+22,&max,8);memcpy(p+38,&length,8);
    if(CorruptResource) p[3]=1;
    *resources=p;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE kind,EFI_GUID *guid,VOID *key,UINTN *count,EFI_HANDLE **handles) {
    UINTN i;(void)key;CHECK(kind==ByProtocol);
    if(BytesEqual(guid,&PciGuid,16)) {
        if(Scenario==6) {*count=513;*handles=NULL;return EFI_SUCCESS;}
        *count=Scenario==2?3:2;
        *handles=malloc(*count*sizeof(EFI_HANDLE));CHECK(*handles!=NULL);
        for(i=0;i<*count;i++) (*handles)[i]=(EFI_HANDLE)(i==2?1:i+1);
        return EFI_SUCCESS;
    }
    CHECK(BytesEqual(guid,&GopGuid,16));
    if(Scenario==7) {*count=0;*handles=NULL;return EFI_NOT_FOUND;}
    *count=2;*handles=malloc(2*sizeof(EFI_HANDLE));CHECK(*handles!=NULL);
    (*handles)[0]=(EFI_HANDLE)(UINTN)3;(*handles)[1]=(EFI_HANDLE)(UINTN)4;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *guid,VOID **out) {
    UINTN id=(UINTN)h;
    if(BytesEqual(guid,&LoadedGuid,16)&&h==Image) {*out=&Loaded;return EFI_SUCCESS;}
    if(BytesEqual(guid,&FsGuid,16)&&h==Volume) {*out=&Fs;return EFI_SUCCESS;}
    if(BytesEqual(guid,&PciGuid,16)&&id>=1&&id<=2) {*out=&MockPci[id-1];return EFI_SUCCESS;}
    if(BytesEqual(guid,&DpGuid,16)) {
        switch(id) {case 1:*out=TargetPath;return EFI_SUCCESS;case 2:*out=IntelPath;return EFI_SUCCESS;
        case 3:*out=TargetGopPath;return EFI_SUCCESS;case 4:*out=IntelGopPath;return EFI_SUCCESS;}
        if(h==Volume) {*out=NonUsb?IntelPath:UsbVolumePath;return EFI_SUCCESS;}
    }
    if(BytesEqual(guid,&GopGuid,16)&&id>=3&&id<=4) {*out=&MockGop[id-3];return EFI_SUCCESS;}
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI PoolFree(VOID *p) {free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Stall(UINTN delay) {CHECK(delay==8000000);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI GetVar(CHAR16 *name,EFI_GUID *guid,UINT32 *attr,UINTN *size,VOID *out) {
    (void)name;CHECK(BytesEqual(guid,&GlobalGuid,16));*attr=0;
    if(*size==1) {*(UINT8 *)out=0;return EFI_SUCCESS;}
    return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI CloseFile(EFI_FILE_PROTOCOL *self) {
    if(self==&Root) RootCloses++;else if(self==&Directory) DirCloses++;
    else if(self==&Log) LogCloses++;else CHECK(self==&Existing);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenFile(EFI_FILE_PROTOCOL *self,EFI_FILE_PROTOCOL **out,CHAR16 *name,UINT64 mode,UINT64 attr) {
    if(self==&Root) {CHECK(name[0]==L'A'&&attr==EFI_FILE_DIRECTORY);*out=&Directory;return EFI_SUCCESS;}
    CHECK(self==&Directory&&name[0]==L'C');
    if(mode==EFI_FILE_MODE_READ) {
        ExistingOpens++;
        if(ExistingFirst&&ExistingOpens==1) {*out=&Existing;return EFI_SUCCESS;}
        return EFI_NOT_FOUND;
    }
    CHECK(mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE)&&attr==0);
    if(FailedLog) return EFI_WRITE_PROTECTED;
    *out=&Log;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,EFI_FILE_PROTOCOL **out) {
    CHECK(self==&Fs);*out=&Root;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI WriteLog(EFI_FILE_PROTOCOL *self,UINTN *size,VOID *buffer) {
    CHECK(self==&Log&&*size<sizeof(Written));FileWrites++;
    memcpy(Written,buffer,*size);Written[*size]=0;
    if(ShortWrite) --*size;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FlushLog(EFI_FILE_PROTOCOL *self) {CHECK(self==&Log);return FailedFlush?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static void Put16(UINT8 *p,UINT16 v) {p[0]=(UINT8)(v&0xff);p[1]=(UINT8)(v>>8);}
static void Setup(unsigned scenario) {
    unsigned i;
    memset(&Bs,0,sizeof(Bs));memset(&Rs,0,sizeof(Rs));memset(&St,0,sizeof(St));memset(MockPci,0,sizeof(MockPci));
    memset(Config,0,sizeof(Config));memset(&Root,0,sizeof(Root));memset(&Directory,0,sizeof(Directory));memset(&Log,0,sizeof(Log));memset(&Existing,0,sizeof(Existing));
    memset(Rom,0,sizeof(Rom));memset(Written,0,sizeof(Written));memset(Displays,0,sizeof(Displays));
    Rom[0]=0x55;Rom[1]=0xaa;Rom[0x18]=0x40;memcpy(Rom+0x40,"PCIR",4);
    Rom[0x44]=0xde;Rom[0x45]=0x10;Put16(Rom+0x46,PROBE_DEVICE);Rom[0x4a]=0x18;Rom[0x50]=1;Rom[0x55]=0x80;
    Config[0][0]=0xde;Config[0][1]=0x10;Put16(Config[0]+2,PROBE_DEVICE);
    Config[0][11]=3;Put16(Config[0]+44,PROBE_SUBVENDOR);Put16(Config[0]+46,PROBE_SUBDEVICE);
    Config[0][4]=3;Config[0][8]=0xa1;{UINT32 bar0=PROBE_BAR0;memcpy(Config[0]+16,&bar0,4);}
    Config[1][0]=0x86;Config[1][1]=0x80;Config[1][2]=1;Config[1][11]=3;
    if(scenario==1) Config[0][46]^=1;
    if(scenario==3) Config[0][2]=0x05,Config[0][3]=0x27;
    for(i=0;i<2;i++) {MockPci[i].Pci.Read=ConfigRead;MockPci[i].GetLocation=Location;MockPci[i].GetBarAttributes=Bar;MockGop[i].Mode=&GopMode;}
    MockPci[0].RomImage=Rom;MockPci[0].RomSize=sizeof(Rom);
    MockPci[0].Mem.Read=MemoryRead;
    GopMode.Info=&GopInfo;GopMode.SizeOfInfo=sizeof(GopInfo);GopInfo.HorizontalResolution=1920;GopInfo.VerticalResolution=1080;
    Bs.HandleProtocol=Handle;Bs.LocateHandleBuffer=Locate;Bs.FreePool=PoolFree;Bs.Stall=Stall;
    Rs.GetVariable=GetVar;St.BootServices=&Bs;St.RuntimeServices=&Rs;St.ConsoleOutHandle=(EFI_HANDLE)(UINTN)3;
    St.ConsoleInHandle=(EFI_HANDLE)(UINTN)99;Loaded.DeviceHandle=Volume;Fs.OpenVolume=OpenVolume;
    Root.Open=OpenFile;Root.Close=CloseFile;Directory.Open=OpenFile;Directory.Close=CloseFile;
    Log.Write=WriteLog;Log.Close=CloseFile;Log.Flush=FlushLog;Existing.Close=CloseFile;
    Scenario=scenario;ConfigReads=BarReads=FileWrites=ExistingOpens=RootCloses=DirCloses=LogCloses=0;
    FailedConfig=FailedLocation=FailedLog=ShortWrite=FailedFlush=ExistingFirst=NonUsb=FALSE;
    MemoryReads=0;FailMemory=CorruptResource=ChangeIdentity=FALSE;
    memset(MockProm,0,sizeof(MockProm));MockProm[0]=0xaa55;FailProm=FALSE;
}
static void TestCore(void) {
    UINT8 digest[32],bad[64],million[1000];CHAR8 buf[67];TEXT t;UINTN n;BOOLEAN multi;unsigned i;
    Sha256((const UINT8 *)"",0,digest);TextInit(&t,buf,sizeof(buf));TextBytes(&t,digest,32);
    CHECK(!t.failed&&strcmp(buf,"\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"")==0);
    Sha256((const UINT8 *)"abc",3,digest);TextInit(&t,buf,sizeof(buf));TextBytes(&t,digest,32);
    CHECK(strcmp(buf,"\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"")==0);
    for(i=0;i<1000;i++) million[i]='a';
    Sha256(million,1000,digest);TextInit(&t,buf,sizeof(buf));TextBytes(&t,digest,32);
    CHECK(strcmp(buf,"\"41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3\"")==0);
    TextInit(&t,buf,2);TextPut(&t,"abc");CHECK(t.failed&&t.len==1&&buf[1]==0);
    CHECK(DpMeasure(TargetPath,sizeof(TargetPath),&n,&multi)&&n==sizeof(TargetPath)&&!multi);
    CHECK(DpParent(TargetPath,sizeof(TargetPath),TargetGopPath,sizeof(TargetGopPath)));
    CHECK(!DpParent(TargetPath,sizeof(TargetPath),IntelGopPath,sizeof(IntelGopPath)));
    CHECK(DpHasUsb(UsbVolumePath,sizeof(UsbVolumePath)));CHECK(!DpHasUsb(IntelPath,sizeof(IntelPath)));
    for(i=0;i<sizeof(TargetPath);i++) CHECK(!DpMeasure(TargetPath,i,&n,&multi));
    memcpy(bad,TargetPath,sizeof(TargetPath));bad[2]=0;CHECK(!DpMeasure(bad,sizeof(TargetPath),&n,&multi));
    bad[2]=0xff;bad[3]=0xff;CHECK(!DpMeasure(bad,sizeof(TargetPath),&n,&multi));
    memcpy(bad,TargetPath,sizeof(TargetPath));bad[7]=1;memcpy(bad+10,TargetPath,sizeof(TargetPath));
    CHECK(DpMeasure(bad,20,&n,&multi)&&multi);CHECK(!DpParent(bad,20,TargetGopPath,sizeof(TargetGopPath)));
    Setup(0);CHECK(ExactTarget(Config[0],64));CHECK(!ExactTarget(Config[0],63));Config[0][14]=1;CHECK(!ExactTarget(Config[0],64));
    CHECK(RomPcirValid(Rom,512,0x10de,PROBE_DEVICE));Rom[0x50]=0;CHECK(!RomPcirValid(Rom,512,0x10de,PROBE_DEVICE));
    Rom[0x50]=1;Rom[0x55]=0;CHECK(!RomPcirValid(Rom,512,0x10de,PROBE_DEVICE));
    Rom[0x55]=0x80;Rom[0x18]=0xff;Rom[0x19]=0xff;CHECK(!RomPcirValid(Rom,512,0x10de,PROBE_DEVICE));
}
static void TestApplication(void) {
    unsigned scenario;
    for(scenario=0;scenario<=7;scenario++) {
        Setup(scenario);CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CHECK(FileWrites==1&&RootCloses==1&&DirCloses==1&&LogCloses==1);
        CHECK(strstr(Written,"\"gpu_writes\":false")&&strstr(Written,"\"unlock_attempted\":false"));
        if(scenario==0) {
            CHECK(strstr(Written,"\"unique_expected_identity_observed\":true"));
            CHECK(strstr(Written,"\"display_parent_index\":0")&&strstr(Written,"\"display_parent_index\":1"));
            CHECK(MemoryReads==25&&strstr(Written,"\"mmio_reads\":true"));
            {FILE *f=fopen(HOST_CAPTURE,"wb");CHECK(f!=NULL);CHECK(fwrite(Written,1,strlen(Written),f)==strlen(Written));CHECK(fclose(f)==0);}
        }
        if(scenario==1||scenario==2||scenario==3||scenario==6) CHECK(strstr(Written,"\"unique_expected_identity_observed\":false"));
    }
    Setup(0);FailedConfig=TRUE;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CHECK(strstr(Written,"\"pci_enumeration_complete\":false"));
    Setup(0);FailedLocation=TRUE;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CHECK(strstr(Written,"\"unique_expected_identity_observed\":false"));
    Setup(0);FailedLog=TRUE;CHECK(UefiMain(Image,&St)==EFI_WRITE_PROTECTED);CHECK(ConfigReads==0&&FileWrites==0);
    Setup(0);NonUsb=TRUE;CHECK(UefiMain(Image,&St)==EFI_ACCESS_DENIED);CHECK(ConfigReads==0&&FileWrites==0&&RootCloses==0);
    Setup(0);ShortWrite=TRUE;CHECK(UefiMain(Image,&St)==EFI_DEVICE_ERROR);CHECK(LogCloses==1);
    Setup(0);FailedFlush=TRUE;CHECK(UefiMain(Image,&St)==EFI_DEVICE_ERROR);CHECK(LogCloses==1);
    Setup(0);ExistingFirst=TRUE;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CHECK(ExistingOpens==2);
    CHECK(UefiMain(Image,NULL)==EFI_INVALID_PARAMETER);
}
static void TestSampler(void) {
    CHAR8 buffer[8192];TEXT t;UINTN reads;
    Setup(0);TextInit(&t,buffer,sizeof(buffer));reads=SampleRegisters(&t,&MockPci[0],&Bs);
    CHECK(reads==23&&MemoryReads==23&&strstr(buffer,"\"complete\":true"));
    Setup(0);Config[0][4]=7;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleRegisters(&t,&MockPci[0],&Bs)==0&&MemoryReads==0);
    Setup(0);Config[0][4]=1;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleRegisters(&t,&MockPci[0],&Bs)==0&&MemoryReads==0);
    Setup(0);Config[0][19]=0x90;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleRegisters(&t,&MockPci[0],&Bs)==0&&MemoryReads==0);
    Setup(0);CorruptResource=TRUE;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleRegisters(&t,&MockPci[0],&Bs)==0&&MemoryReads==0);
    Setup(0);FailedLocation=TRUE;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleRegisters(&t,&MockPci[0],&Bs)==0&&MemoryReads==0);
    Setup(0);Config[0][46]^=1;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleRegisters(&t,&MockPci[0],&Bs)==0&&MemoryReads==0);
    Setup(0);FailMemory=TRUE;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleRegisters(&t,&MockPci[0],&Bs)==2&&MemoryReads==2&&strstr(buffer,"\"complete\":false"));
    Setup(0);ChangeIdentity=TRUE;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleRegisters(&t,&MockPci[0],&Bs)==22&&MemoryReads==22&&strstr(buffer,"\"pci_snapshot_unchanged\":false"));
    Setup(0);Config[0][8]=0xa2;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleRegisters(&t,&MockPci[0],&Bs)==0&&MemoryReads==0);
}
static void TestProm(void) {
    static CHAR8 buffer[3*1024*1024];TEXT t;
    Setup(0);TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleProm(&t,&MockPci[0],&Bs)==259&&!t.failed&&strstr(buffer,"\"complete\":true"));
    Setup(0);MockProm[0]=0;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleProm(&t,&MockPci[0],&Bs)==2&&strstr(buffer,"\"prom_signature_unavailable\""));
    Setup(0);Config[0][4]=7;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleProm(&t,&MockPci[0],&Bs)==0&&MemoryReads==0);
    Setup(0);CorruptResource=TRUE;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleProm(&t,&MockPci[0],&Bs)==0&&MemoryReads==0);
    Setup(0);FailProm=TRUE;TextInit(&t,buffer,sizeof(buffer));
    CHECK(SampleProm(&t,&MockPci[0],&Bs)==4&&strstr(buffer,"\"hex\":null")&&strstr(buffer,"\"complete\":false"));
}
int main(void) {TestCore();TestApplication();TestSampler();TestProm();printf("PASS: %u checks; full EFI application executed against host mocks; no GPU hardware accessed.\n",Checks);return 0;}
