/* SPDX-License-Identifier: MIT */
#include "../src/probe.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <Protocol/DriverBinding.h>

static unsigned Checks;
#define CHECK(x) do {Checks++; if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}} while(0)
static UINT8 TargetPath[]={1,1,6,0,0,1,0x7f,0xff,4,0};
static UINT8 IntelPath[]={1,1,6,0,0,2,0x7f,0xff,4,0};
static UINT8 TargetGopPath[]={1,1,6,0,0,1,2,3,8,0,0,0,0,0,0x7f,0xff,4,0};
static UINT8 IntelGopPath[]={1,1,6,0,0,2,2,3,8,0,0,0,0,0,0x7f,0xff,4,0};
static UINT8 UsbVolumePath[]={3,5,6,0,0,0,0x7f,0xff,4,0};
/* Synthetic PciRoot(0)/Pci(0x1b,0)/NVMe(1)/HD(1,GPT,...,0x800,0x200000). */
static UINT8 FixedVolumePath[]={
    2,1,12,0,0xd0,0x41,3,0x0a,0,0,0,0,
    1,1,6,0,0,0x1b,
    3,0x17,16,0,1,0,0,0,0,0,0,0,0,0,0,0,
    4,1,42,0,1,0,0,0,0,8,0,0,0,0,0,0,0,0,0x20,0,0,0,0,0,
    0x33,0x22,0x11,0,0x55,0x44,0x77,0x66,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,2,2,
    0x7f,0xff,4,0
};
static UINT8 *SourcePath;
static EFI_PCI_IO_PROTOCOL MockPci[2];
static EFI_GRAPHICS_OUTPUT_PROTOCOL MockGop[2];
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE GopMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION GopInfo;
static EFI_BOOT_SERVICES Bs;
static EFI_RUNTIME_SERVICES Rs;
static EFI_SYSTEM_TABLE St;
static EFI_LOADED_IMAGE_PROTOCOL Loaded,DriverImage;
static EFI_DRIVER_BINDING_PROTOCOL Binding;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL TextOut;
static EFI_GUID DriverGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
static EFI_FILE_PROTOCOL Root,Directory,Log,Existing;
#define TEST_ROM_MAX (1024u*1024u)
#define TEST_ROM_SIZE 1536u
static UINT8 Config[2][4096],Rom[TEST_ROM_MAX],DriverPe[4096],LongPath[4096];
static UINT32 MockProm[TEST_ROM_MAX/4];
static const UINT32 RegisterOffsets[]={0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,0x823814,0x823818,0x82381c,0x823820,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0,0x840100};
static EFI_HANDLE Image=(EFI_HANDLE)(UINTN)100,Volume=(EFI_HANDLE)(UINTN)101;
static unsigned Scenario,ConfigReads,ExtendedReads,LocationReads,BarReads,FileWrites,ExistingOpens,ExistingCloses,RootCloses,DirCloses,LogCloses;
static unsigned ForbiddenCalls,GraphReads,ProtocolReads,OpenInfoReads,ConsoleWrites,CreatedSlot;
static unsigned PciEnumerations,FsRequests,VolumeOpens;
static unsigned FullConfigReads,MemoryReads,RegisterReads,PromReads,ChipReads,RegisterSeen[22],PromPages[256];
static unsigned FailMemoryAt,SentinelAt,ChangeConfigAt,ChangeChipAt,FailConfigAfterMemory,ResourceFault,LocationFault,FullConfigDrift;
static UINT32 ChipValue,SentinelValue;
static BOOLEAN FailedFullConfig,FailProm,ChangePromSignature;
static char Written[sizeof(Report)];
static BOOLEAN FailedConfig,FailedExtended,ChangeExtended,FailedLocation,FailedLog,FailedWrite,ShortWrite,FailedFlush,ExistingFirst,ExistingAll,MissingSourceFs;

/* Typed non-null traps ensure a forbidden call fails even behind a pointer check. */
#pragma warning(push)
#pragma warning(disable:4100)
static EFI_STATUS Forbidden(const char *name) {ForbiddenCalls++;fprintf(stderr,"Forbidden callback: %s\n",name);return EFI_ACCESS_DENIED;}
static EFI_STATUS EFIAPI MemoryAccess(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT8 b,UINT64 o,UINTN n,VOID *out) {return Forbidden("Mem/Io");}
static EFI_STATUS EFIAPI ConfigWrite(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 o,UINTN n,VOID *out) {return Forbidden("Pci.Write");}
static EFI_STATUS EFIAPI Poll(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT8 b,UINT64 o,UINT64 m,UINT64 v,UINT64 d,UINT64 *r) {return Forbidden("PollMem/PollIo");}
static EFI_STATUS EFIAPI Copy(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT8 db,UINT64 d,UINT8 sb,UINT64 s,UINTN n) {return Forbidden("CopyMem");}
static EFI_STATUS EFIAPI Map(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_OPERATION o,VOID *h,UINTN *n,EFI_PHYSICAL_ADDRESS *a,VOID **m) {return Forbidden("Map");}
static EFI_STATUS EFIAPI Unmap(EFI_PCI_IO_PROTOCOL *p,VOID *m) {return Forbidden("Unmap");}
static EFI_STATUS EFIAPI Allocate(EFI_PCI_IO_PROTOCOL *p,EFI_ALLOCATE_TYPE t,EFI_MEMORY_TYPE mt,UINTN n,VOID **h,UINT64 a) {return Forbidden("AllocateBuffer");}
static EFI_STATUS EFIAPI FreeBuffer(EFI_PCI_IO_PROTOCOL *p,UINTN n,VOID *h) {return Forbidden("FreeBuffer");}
static EFI_STATUS EFIAPI Attributes(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION o,UINT64 a,UINT64 *r) {return Forbidden("Attributes");}
static EFI_STATUS EFIAPI SetBar(EFI_PCI_IO_PROTOCOL *p,UINT64 a,UINT8 b,UINT64 *o,UINT64 *n) {return Forbidden("SetBarAttributes");}
static EFI_STATUS EFIAPI SetVar(CHAR16 *n,EFI_GUID *g,UINT32 a,UINTN z,VOID *d) {return Forbidden("SetVariable");}
static VOID EFIAPI Reset(EFI_RESET_TYPE t,EFI_STATUS s,UINTN n,VOID *d) {(void)Forbidden("ResetSystem");}
static EFI_STATUS EFIAPI Connect(EFI_HANDLE h,EFI_HANDLE *d,EFI_DEVICE_PATH_PROTOCOL *p,BOOLEAN r) {return Forbidden("ConnectController");}
static EFI_STATUS EFIAPI Disconnect(EFI_HANDLE h,EFI_HANDLE d,EFI_HANDLE c) {return Forbidden("DisconnectController");}
static EFI_STATUS EFIAPI Load(BOOLEAN b,EFI_HANDLE h,EFI_DEVICE_PATH_PROTOCOL *p,VOID *s,UINTN n,EFI_HANDLE *o) {return Forbidden("LoadImage");}
static EFI_STATUS EFIAPI Start(EFI_HANDLE h,UINTN *n,CHAR16 **d) {return Forbidden("StartImage");}
static EFI_STATUS EFIAPI Unload(EFI_HANDLE h) {return Forbidden("UnloadImage");}
static EFI_STATUS EFIAPI ExitBoot(EFI_HANDLE h,UINTN k) {return Forbidden("ExitBootServices");}
static EFI_STATUS EFIAPI CloseProtocol(EFI_HANDLE h,EFI_GUID *g,EFI_HANDLE a,EFI_HANDLE c) {return Forbidden("CloseProtocol");}
static EFI_STATUS EFIAPI DriverStart(EFI_DRIVER_BINDING_PROTOCOL *p,EFI_HANDLE h,EFI_DEVICE_PATH_PROTOCOL *d) {return Forbidden("DriverBinding.Start/Supported");}
static EFI_STATUS EFIAPI DriverStop(EFI_DRIVER_BINDING_PROTOCOL *p,EFI_HANDLE h,UINTN n,EFI_HANDLE *c) {return Forbidden("DriverBinding.Stop");}
static EFI_STATUS EFIAPI SetMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *p,UINT32 m) {return Forbidden("Gop.SetMode");}
static EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *p,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *b,EFI_GRAPHICS_OUTPUT_BLT_OPERATION o,UINTN sx,UINTN sy,UINTN dx,UINTN dy,UINTN w,UINTN h,UINTN d) {return Forbidden("Gop.Blt");}
#pragma warning(pop)

static EFI_STATUS EFIAPI ConfigRead(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 off,UINTN count,VOID *out) {
    CHECK(width==EfiPciIoWidthUint32&&off==0&&(count==16||count==64||count==1024));ConfigReads++;
    if(FailedConfig && self==&MockPci[0]) return EFI_DEVICE_ERROR;
    if(FailConfigAfterMemory&&MemoryReads>=FailConfigAfterMemory) return EFI_DEVICE_ERROR;
    if(count==64) {ExtendedReads++;if(FailedExtended) return EFI_DEVICE_ERROR;}
    if(count==1024) {CHECK(self==&MockPci[0]);FullConfigReads++;}
    memcpy(out,Config[self==&MockPci[0]?0:1],count*4);
    if(count==64&&ChangeExtended) ((UINT8 *)out)[8]^=1;
    if(count==1024&&FullConfigDrift) ((UINT8 *)out)[FullConfigDrift-1]^=1;
    if(count==1024&&FailedFullConfig) return EFI_DEVICE_ERROR; /* Dirty buffer must not be exported. */
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MemoryRead(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,UINT8 bar,UINT64 offset,UINTN count,VOID *out) {
    UINTN i;BOOLEAN found=FALSE;
    CHECK(self==&MockPci[0]&&width==EfiPciIoWidthUint32&&bar==0&&out!=NULL);
    MemoryReads++;
    if(offset>=0x300000&&offset<0x400000) {
        CHECK((count==1||count==1024)&&offset+count*4<=0x400000&&!(offset&3));
        if(count==1) CHECK(offset==0x300000);
        else {CHECK(!(offset&4095));PromPages[(offset-0x300000)/4096]++;}
        CHECK(ChipReads>0&&ChipValue==0x194000a1u);PromReads++;
        if((FailProm&&offset==0x301000)||FailMemoryAt==MemoryReads) return EFI_DEVICE_ERROR;
        memcpy(out,(UINT8 *)MockProm+(offset-0x300000),count*4);
        if(ChangePromSignature&&offset==0x300000&&count==1024) ((UINT32 *)out)[0]^=1;
    } else {
        CHECK(count==1);
        for(i=0;i<sizeof(RegisterOffsets)/sizeof(RegisterOffsets[0]);i++) if(offset==RegisterOffsets[i]) {found=TRUE;RegisterSeen[i]++;}
        CHECK(found);RegisterReads++;
        if(offset==0) ChipReads++;
        else CHECK(ChipReads>0&&ChipValue==0x194000a1u);
        if(FailMemoryAt==MemoryReads) return EFI_DEVICE_ERROR;
        *(UINT32 *)out=offset==0?ChipValue:(UINT32)(offset^0x00010001u);
        if(ChangeChipAt&&ChipReads==ChangeChipAt&&offset==0) *(UINT32 *)out^=1;
        if(SentinelAt==MemoryReads) *(UINT32 *)out=SentinelValue;
    }
    if(ChangeConfigAt==MemoryReads) Config[0][8]^=1;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Location(EFI_PCI_IO_PROTOCOL *self,UINTN *seg,UINTN *bus,UINTN *dev,UINTN *fun) {
    LocationReads++;
    if(FailedLocation) return EFI_DEVICE_ERROR;
    *seg=0;*bus=self==&MockPci[0]?1:0;*dev=self==&MockPci[0]?0:2;*fun=0;
    if(LocationFault==1) *seg=1;if(LocationFault==2) *bus=2;if(LocationFault==3) *dev=1;if(LocationFault==4) *fun=1;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Bar(EFI_PCI_IO_PROTOCOL *self,UINT8 index,UINT64 *supports,VOID **resources) {
    UINT8 *p; (void)self;CHECK(index<6);BarReads++;*supports=0;
    p=calloc(1,46);CHECK(p!=NULL);p[0]=0x8a;p[1]=43;p[3]=0;p[17]=0x80;p[41]=1;
    p[22]=0xff;p[23]=0xff;p[24]=0xff;p[25]=0x80;
    if(ResourceFault==1) p[3]=1;
    if(ResourceFault==2) p[17]=0x90;
    if(ResourceFault==3) p[30]=1;
    if(ResourceFault==4) p[41]=2;
    if(ResourceFault==5) p[25]=0x81;
    if(ResourceFault==6) p[0]=0;
    if(ResourceFault==7) p[1]=42;
    if(ResourceFault==8) {free(p);*resources=NULL;return EFI_DEVICE_ERROR;}
    *resources=p;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE kind,EFI_GUID *guid,VOID *key,UINTN *count,EFI_HANDLE **handles) {
    UINTN i;(void)key;
    if(kind==AllHandles) {
        CHECK(guid==NULL);GraphReads++;*count=Scenario==8?1100:5;
        *handles=malloc(*count*sizeof(EFI_HANDLE));CHECK(*handles!=NULL);
        for(i=0;i<*count;i++) (*handles)[i]=(EFI_HANDLE)(i<5?i+1:i+200);
        return EFI_SUCCESS;
    }
    CHECK(kind==ByProtocol);
    if(BytesEqual(guid,&PciGuid,16)) {
        PciEnumerations++;
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
    if(BytesEqual(guid,&LoadedGuid,16)&&id==5) {*out=&DriverImage;return EFI_SUCCESS;}
    if(BytesEqual(guid,&DriverGuid,16)&&id==5) {*out=&Binding;return EFI_SUCCESS;}
    if(BytesEqual(guid,&FsGuid,16)) {
        FsRequests++;CHECK(h==Loaded.DeviceHandle); /* Never fall back to another volume. */
        if(MissingSourceFs) return EFI_UNSUPPORTED;
        *out=&Fs;return EFI_SUCCESS;
    }
    if(BytesEqual(guid,&PciGuid,16)&&id>=1&&id<=2) {*out=&MockPci[id-1];return EFI_SUCCESS;}
    if(BytesEqual(guid,&DpGuid,16)) {
        switch(id) {case 1:*out=TargetPath;return EFI_SUCCESS;case 2:*out=IntelPath;return EFI_SUCCESS;
        case 3:*out=TargetGopPath;return EFI_SUCCESS;case 4:*out=IntelGopPath;return EFI_SUCCESS;}
        if(h==Loaded.DeviceHandle) {*out=SourcePath;return EFI_SUCCESS;}
        if(Scenario==8&&id>=200) {*out=LongPath;return EFI_SUCCESS;}
    }
    if(BytesEqual(guid,&GopGuid,16)&&id>=3&&id<=4) {*out=&MockGop[id-3];return EFI_SUCCESS;}
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Protocols(EFI_HANDLE h,EFI_GUID ***out,UINTN *count) {
    (void)h;ProtocolReads++;*count=1;*out=malloc(sizeof(EFI_GUID *));CHECK(*out!=NULL);(*out)[0]=&PciGuid;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenInfo(EFI_HANDLE h,EFI_GUID *guid,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *count) {
    CHECK(BytesEqual(guid,&PciGuid,16));OpenInfoReads++;*count=0;*out=NULL;
    if((UINTN)h==1||(UINTN)h==3) {
        *count=1;*out=calloc(1,sizeof(**out));CHECK(*out!=NULL);
        (*out)[0].AgentHandle=(EFI_HANDLE)(UINTN)5;(*out)[0].ControllerHandle=h;
        (*out)[0].Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;(*out)[0].OpenCount=1;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Output(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,CHAR16 *text) {
    CHECK(self==&TextOut&&text!=NULL);ConsoleWrites++;return EFI_SUCCESS;
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
    else if(self==&Log) LogCloses++;else {CHECK(self==&Existing);ExistingCloses++;}
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenFile(EFI_FILE_PROTOCOL *self,EFI_FILE_PROTOCOL **out,CHAR16 *name,UINT64 mode,UINT64 attr) {
    if(self==&Root) {CHECK(name[0]==L'A'&&attr==EFI_FILE_DIRECTORY);*out=&Directory;return EFI_SUCCESS;}
    CHECK(self==&Directory&&name[0]==L'C'&&name[1]==L'A'&&name[2]==L'P');
    CHECK(name[7]==L'.'&&name[8]==L'J'&&name[9]==L'S'&&name[10]==L'O'&&name[11]==L'N'&&name[12]==0);
    if(mode==EFI_FILE_MODE_READ) {
        ExistingOpens++;
        if(ExistingAll||(ExistingFirst&&ExistingOpens==1)) {*out=&Existing;return EFI_SUCCESS;}
        return EFI_NOT_FOUND;
    }
    CHECK(mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE)&&attr==0);
    CHECK(!ExistingAll);CreatedSlot=(name[3]-L'0')*1000+(name[4]-L'0')*100+(name[5]-L'0')*10+name[6]-L'0';
    CHECK(!ExistingFirst||CreatedSlot!=1);
    if(FailedLog) return EFI_WRITE_PROTECTED;
    *out=&Log;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *self,EFI_FILE_PROTOCOL **out) {
    CHECK(self==&Fs);VolumeOpens++;*out=&Root;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI WriteLog(EFI_FILE_PROTOCOL *self,UINTN *size,VOID *buffer) {
    CHECK(self==&Log&&*size<sizeof(Written));FileWrites++;
    memcpy(Written,buffer,*size);Written[*size]=0;
    if(FailedWrite) return EFI_DEVICE_ERROR;
    if(ShortWrite) --*size;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FlushLog(EFI_FILE_PROTOCOL *self) {CHECK(self==&Log);return FailedFlush?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static void Pe32(UINTN at,UINT32 value) {
    unsigned i;for(i=0;i<4;i++) DriverPe[at+i]=(UINT8)(value>>(i*8));
}
static void Setup(unsigned scenario) {
    unsigned i;
    memset(&Bs,0,sizeof(Bs));memset(&Rs,0,sizeof(Rs));memset(&St,0,sizeof(St));memset(MockPci,0,sizeof(MockPci));
    memset(Config,0,sizeof(Config));memset(&Root,0,sizeof(Root));memset(&Directory,0,sizeof(Directory));memset(&Log,0,sizeof(Log));memset(&Existing,0,sizeof(Existing));
    memset(Rom,0,sizeof(Rom));memset(Written,0,sizeof(Written));memset(Displays,0,sizeof(Displays));
    Rom[0]=0x55;Rom[1]=0xaa;Rom[0x18]=0x40;memcpy(Rom+0x40,"PCIR",4);
    Rom[0x44]=0xde;Rom[0x45]=0x10;Rom[0x46]=0x83;Rom[0x47]=0x27;Rom[0x4a]=0x18;Rom[0x50]=3;Rom[0x55]=0x80;
    Config[0][0]=0xde;Config[0][1]=0x10;Config[0][2]=0x83;Config[0][3]=0x27;
    Config[0][11]=3;Config[0][44]=0x58;Config[0][45]=0x14;Config[0][46]=0x3b;Config[0][47]=0x41;
    Config[0][4]=3;Config[0][8]=0xa1;Config[0][16]=0;Config[0][19]=0x80;
    for(i=64;i<sizeof(Config[0]);i++) Config[0][i]=(UINT8)(i*37+11);
    Config[1][0]=0x86;Config[1][1]=0x80;Config[1][2]=1;Config[1][11]=3;
    if(scenario==1) Config[0][46]^=1;
    if(scenario==3) Config[0][2]=0x05,Config[0][3]=0x27;
    for(i=0;i<2;i++) {
        MockPci[i].Pci.Read=ConfigRead;MockPci[i].Pci.Write=ConfigWrite;MockPci[i].GetLocation=Location;MockPci[i].GetBarAttributes=Bar;
        MockPci[i].Mem.Read=MockPci[i].Mem.Write=MockPci[i].Io.Read=MockPci[i].Io.Write=MemoryAccess;
        MockPci[i].PollMem=MockPci[i].PollIo=Poll;MockPci[i].CopyMem=Copy;
        MockPci[i].Map=Map;MockPci[i].Unmap=Unmap;MockPci[i].AllocateBuffer=Allocate;MockPci[i].FreeBuffer=FreeBuffer;
        MockPci[i].Attributes=Attributes;MockPci[i].SetBarAttributes=SetBar;
        MockGop[i].Mode=&GopMode;MockGop[i].SetMode=SetMode;MockGop[i].Blt=Blt;
    }
    MockPci[0].RomImage=Rom;MockPci[0].RomSize=TEST_ROM_SIZE;
    MockPci[0].Mem.Read=MemoryRead;
    GopMode.Info=&GopInfo;GopMode.SizeOfInfo=sizeof(GopInfo);GopInfo.HorizontalResolution=1920;GopInfo.VerticalResolution=1080;
    Bs.HandleProtocol=Handle;Bs.LocateHandleBuffer=Locate;Bs.FreePool=PoolFree;Bs.Stall=Stall;
    Bs.ProtocolsPerHandle=Protocols;Bs.OpenProtocolInformation=OpenInfo;Bs.CloseProtocol=CloseProtocol;
    Bs.ConnectController=Connect;Bs.DisconnectController=Disconnect;Bs.LoadImage=Load;Bs.StartImage=Start;Bs.UnloadImage=Unload;Bs.ExitBootServices=ExitBoot;
    Rs.GetVariable=GetVar;St.BootServices=&Bs;St.RuntimeServices=&Rs;St.ConsoleOutHandle=(EFI_HANDLE)(UINTN)3;
    Rs.SetVariable=SetVar;Rs.ResetSystem=Reset;TextOut.OutputString=Output;St.ConOut=&TextOut;
    St.ConsoleInHandle=(EFI_HANDLE)(UINTN)99;Loaded.DeviceHandle=Volume;SourcePath=UsbVolumePath;Fs.OpenVolume=OpenVolume;
    Root.Open=OpenFile;Root.Close=CloseFile;Directory.Open=OpenFile;Directory.Close=CloseFile;
    Log.Write=WriteLog;Log.Close=CloseFile;Log.Flush=FlushLog;Existing.Close=CloseFile;
    memset(&Binding,0,sizeof(Binding));memset(&DriverImage,0,sizeof(DriverImage));memset(DriverPe,0,sizeof(DriverPe));
    Binding.DriverBindingHandle=Binding.ImageHandle=(EFI_HANDLE)(UINTN)5;Binding.Version=0x6000b;
    Binding.Stop=DriverStop;Binding.Start=Binding.Supported=DriverStart;
    /* Synthetic loaded PE: one 16-byte executable section, as in platform_tests.c. */
    Pe32(0,0x5a4d);Pe32(60,64);Pe32(64,0x4550);Pe32(68,0x00018664);Pe32(84,240);
    Pe32(88,0x20b);Pe32(88+56,sizeof(DriverPe));Pe32(88+108,6);Pe32(328+8,16);Pe32(328+12,0x200);Pe32(328+36,0x20000000);
    for(i=0;i<16;i++) DriverPe[0x200+i]=(UINT8)i;
    DriverImage.ImageBase=DriverPe;DriverImage.ImageSize=sizeof(DriverPe);DriverImage.ImageCodeType=EfiBootServicesCode;
    DriverImage.DeviceHandle=Volume;DriverImage.FilePath=(EFI_DEVICE_PATH_PROTOCOL *)TargetPath;
    memset(LongPath,0,sizeof(LongPath));LongPath[0]=1;LongPath[1]=1;LongPath[2]=0xfc;LongPath[3]=0x0f;
    LongPath[4092]=0x7f;LongPath[4093]=0xff;LongPath[4094]=4;
    Scenario=scenario;ConfigReads=ExtendedReads=LocationReads=BarReads=FileWrites=ExistingOpens=ExistingCloses=RootCloses=DirCloses=LogCloses=0;
    ForbiddenCalls=GraphReads=ProtocolReads=OpenInfoReads=ConsoleWrites=CreatedSlot=0;
    PciEnumerations=FsRequests=VolumeOpens=0;
    FullConfigReads=MemoryReads=RegisterReads=PromReads=ChipReads=0;memset(RegisterSeen,0,sizeof(RegisterSeen));memset(PromPages,0,sizeof(PromPages));
    FailMemoryAt=SentinelAt=ChangeConfigAt=ChangeChipAt=FailConfigAfterMemory=ResourceFault=LocationFault=FullConfigDrift=0;
    ChipValue=0x194000a1u;SentinelValue=0xbadf0001u;FailedFullConfig=FailProm=ChangePromSignature=FALSE;
    memset(MockProm,0,sizeof(MockProm));MockProm[0]=0xaa55;
    FailedConfig=FailedExtended=ChangeExtended=FailedLocation=FailedLog=FailedWrite=ShortWrite=FailedFlush=ExistingFirst=ExistingAll=MissingSourceFs=FALSE;
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
    CHECK(DpMeasure(UsbVolumePath,sizeof(UsbVolumePath),&n,&multi)&&n==sizeof(UsbVolumePath)&&!multi);
    CHECK(DpMeasure(FixedVolumePath,sizeof(FixedVolumePath),&n,&multi)&&n==sizeof(FixedVolumePath)&&!multi);
    for(i=0;i<sizeof(TargetPath);i++) CHECK(!DpMeasure(TargetPath,i,&n,&multi));
    memcpy(bad,TargetPath,sizeof(TargetPath));bad[2]=0;CHECK(!DpMeasure(bad,sizeof(TargetPath),&n,&multi));
    bad[2]=0xff;bad[3]=0xff;CHECK(!DpMeasure(bad,sizeof(TargetPath),&n,&multi));
    memcpy(bad,TargetPath,sizeof(TargetPath));bad[7]=1;memcpy(bad+10,TargetPath,sizeof(TargetPath));
    CHECK(DpMeasure(bad,20,&n,&multi)&&multi);CHECK(!DpParent(bad,20,TargetGopPath,sizeof(TargetGopPath)));
}
static void TestIdentity(void) {
    static const unsigned fields[]={0,2,44,46,8,11,14};
    unsigned i;
    Setup(0);CHECK(ExactTarget(Config[0],64));CHECK(ExactTarget(Config[0],256));
    for(i=0;i<64;i++) CHECK(!ExactTarget(Config[0],i));
    CHECK(!ExactTarget(NULL,0));CHECK(!ExactTarget(NULL,64));
    for(i=0;i<sizeof(fields)/sizeof(fields[0]);i++) {
        Setup(0);Config[0][fields[i]]^=1;CHECK(!ExactTarget(Config[0],64));
    }
    Setup(0);Config[0][14]=0x80;CHECK(ExactTarget(Config[0],64)); /* multifunction type 0 */
}
static void TestRomParser(void) {
    Setup(0);CHECK(RomPcirValid(Rom,TEST_ROM_SIZE,0x10de,0x2783));
    Rom[0x4a]=0xff;Rom[0x4b]=0xff;CHECK(!RomPcirValid(Rom,TEST_ROM_SIZE,0x10de,0x2783));
    Setup(0);Rom[0x50]=1;CHECK(RomPcirValid(Rom,TEST_ROM_SIZE,0x10de,0x2783)); /* cached padding is permitted */
    Rom[0x4a]=0;Rom[0x4b]=2;CHECK(!RomPcirValid(Rom,TEST_ROM_SIZE,0x10de,0x2783)); /* fits buffer, exceeds its 512-byte image */
    Setup(0);Rom[0x4a]=0x17;CHECK(!RomPcirValid(Rom,TEST_ROM_SIZE,0x10de,0x2783));
    Setup(0);Rom[0x50]=0;CHECK(!RomPcirValid(Rom,TEST_ROM_SIZE,0x10de,0x2783));
    Setup(0);Rom[0x55]=0;CHECK(!RomPcirValid(Rom,TEST_ROM_SIZE,0x10de,0x2783));
    Setup(0);Rom[0x18]=0xff;Rom[0x19]=0xff;CHECK(!RomPcirValid(Rom,TEST_ROM_SIZE,0x10de,0x2783));
    Setup(0);Rom[0x46]=0x82;CHECK(!RomPcirValid(Rom,TEST_ROM_SIZE,0x10de,0x2783));
    CHECK(!RomPcirValid(NULL,TEST_ROM_SIZE,0x10de,0x2783));CHECK(!RomPcirValid(Rom,511,0x10de,0x2783));
    Setup(0);CHECK(PROBE_ROM_MAX==TEST_ROM_MAX);
    Rom[0x50]=0;Rom[0x51]=8;CHECK(RomPcirValid(Rom,TEST_ROM_MAX,0x10de,0x2783));
    /* Invalid pointer proves over-limit inputs are rejected before dereferencing. */
    CHECK(!RomPcirValid((UINT8 *)(UINTN)1,TEST_ROM_MAX+1,0x10de,0x2783));
}
static void NoPayload(void) {
    CHECK(strstr(Written,"\"unique_expected_identity_observed\":false"));
    CHECK(strstr(Written,"\"config256\":null")&&strstr(Written,"\"cached_rom_hex\":null"));
    CHECK(strstr(Written,"\"config4096\":null")&&strstr(Written,"\"config4096_status\":\"0x800000000000000e\""));
    CHECK(strstr(Written,"\"mmio_reads\":false")&&ExtendedReads==0&&FullConfigReads==0&&MemoryReads==0&&ForbiddenCalls==0);
    CHECK(GraphReads==0&&ProtocolReads==0&&OpenInfoReads==0);
    CHECK(strstr(Written,"\"platform\":{\"available\":false"));
}
static void SaveCapture(const char *path) {
    FILE *f=fopen(path,"wb");CHECK(f!=NULL);CHECK(fwrite(Written,1,strlen(Written),f)==strlen(Written));CHECK(fclose(f)==0);
}
static void TestApplication(void) {
    static const unsigned fields[]={0,2,44,46,8,11,14};
    unsigned scenario,i;
    Setup(0);CHECK(UefiMain(Image,&St)==EFI_SUCCESS);
    CHECK(FileWrites==1&&RootCloses==1&&DirCloses==1&&LogCloses==1&&CreatedSlot==1);
    CHECK(strstr(Written,"\"unique_expected_identity_observed\":true"));
    CHECK(MemoryReads==282&&RegisterReads==25&&PromReads==257&&FullConfigReads==1);
    CHECK(strstr(Written,"\"probe_version\":\"0.5.0-4070s-readonly-cold\"")&&strstr(Written,"\"probe_mode\":\"readonly-cold-4070s\""));
    CHECK(strstr(Written,"\"gpu_writes\":false")&&strstr(Written,"\"unlock_attempted\":false")&&strstr(Written,"\"mmio_reads\":true"));
    CHECK(strstr(Written,"\"hardware_support_validated\":false")&&ForbiddenCalls==0&&ExtendedReads==1);
    CHECK(strstr(Written,"\"display_parent_index\":0")&&strstr(Written,"\"display_parent_index\":1"));
    CHECK(GraphReads==1&&ProtocolReads==5&&OpenInfoReads==5&&ConsoleWrites>0);
    CHECK(strstr(Written,"\"platform\":{\"available\":true")&&strstr(Written,"\"edge_count\":2"));
    CHECK(strstr(Written,"\"release_authorized\":false")&&strstr(Written,"\"atomic_snapshot\":false"));
    CHECK(strstr(Written,"\"hex\":\"000102030405060708090a0b0c0d0e0f\""));
    CHECK(!strstr(Written,"\"config256\":null")&&!strstr(Written,"\"cached_rom_hex\":null"));
    CHECK(strstr(Written,"\"config4096_status\":\"0x0000000000000000\"")&&!strstr(Written,"\"config4096\":null"));
    SaveCapture("build/host-capture.json");
    for(i=0;i<sizeof(fields)/sizeof(fields[0]);i++) {
        Setup(0);Config[0][fields[i]]^=1;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);NoPayload();
    }
    for(scenario=1;scenario<=7;scenario++) {
        Setup(scenario);CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CHECK(FileWrites==1&&LogCloses==1&&ForbiddenCalls==0);
        if(scenario==1||scenario==2||scenario==3||scenario==6) NoPayload();
    }
    Setup(0);memcpy(Config[1],Config[0],256);Config[1][46]^=1;
    CHECK(UefiMain(Image,&St)==EFI_SUCCESS);NoPayload(); /* one exact plus another same device ID */
    Setup(0);FailedConfig=TRUE;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CHECK(strstr(Written,"\"pci_enumeration_complete\":false"));NoPayload();
    Setup(0);FailedLocation=TRUE;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);NoPayload();
    Setup(0);FailedExtended=TRUE;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CHECK(strstr(Written,"\"config256\":null"));
    Setup(0);ChangeExtended=TRUE;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CHECK(strstr(Written,"\"config256\":null"));
    CHECK(ForbiddenCalls==0);CHECK(UefiMain(Image,NULL)==EFI_INVALID_PARAMETER);
}
static void CheckFullConfig(BOOLEAN present) {
    static const char alphabet[]="0123456789abcdef";UINTN i;const char *p=strstr(Written,"\"config4096\":");
    CHECK(FullConfigReads==1&&p!=NULL&&ForbiddenCalls==0);p+=strlen("\"config4096\":");
    if(!present) {CHECK(strncmp(p,"null",4)==0);return;}
    CHECK(*p++=='"');
    for(i=0;i<sizeof(Config[0]);i++) CHECK(p[2*i]==alphabet[Config[0][i]>>4]&&p[2*i+1]==alphabet[Config[0][i]&15]);
    CHECK(p[2*i]=='"');
}
static void TestFullConfig(void) {
    static const unsigned drift[]={1,5,9,20,45,64,65,129,256};unsigned i;
    Setup(0);CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CheckFullConfig(TRUE);
    CHECK(strstr(Written,"\"config4096_status\":\"0x0000000000000000\""));
    Setup(0);FailedFullConfig=TRUE;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CheckFullConfig(FALSE);
    CHECK(strstr(Written,"\"config4096_status\":\"0x8000000000000007\""));
    SaveCapture("build/host-capture-config-error.json");
    for(i=0;i<sizeof(drift)/sizeof(drift[0]);i++) {
        Setup(0);FullConfigDrift=drift[i];CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CheckFullConfig(FALSE);
        CHECK(strstr(Written,"\"config4096_status\":")&&!strstr(Written,"\"config4096_status\":\"0x0000000000000000\""));
        if(drift[i]==9) SaveCapture("build/host-capture-config-drift.json");
    }
    Setup(0);FailedExtended=TRUE;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CheckFullConfig(TRUE);
    CHECK(strstr(Written,"\"config256\":null"));
}
static void TestPartialApplication(void) {
    Setup(0);FailMemoryAt=2;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);
    CHECK(MemoryReads==261&&strstr(Written,"\"mmio_reads\":true")&&ForbiddenCalls==0);
    SaveCapture("build/host-capture-register-error.json");
    Setup(0);FailProm=TRUE;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);
    CHECK(MemoryReads==27&&strstr(Written,"\"hex\":null")&&ForbiddenCalls==0);
    SaveCapture("build/host-capture-prom-error.json");
    Setup(0);Config[0][4]=1;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);
    CHECK(MemoryReads==0&&strstr(Written,"\"mmio_reads\":false")&&ForbiddenCalls==0);
    SaveCapture("build/host-capture-decode-disabled.json");
    Setup(0);ChipValue=0x194000a2u;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);
    CHECK(MemoryReads==2&&ChipReads==2&&PromReads==0&&ForbiddenCalls==0);
    SaveCapture("build/host-capture-wrong-chip.json");
}
static void SetupLog(unsigned fixed) {
    Setup(0);
    if(fixed) {Loaded.DeviceHandle=(EFI_HANDLE)(UINTN)102;SourcePath=FixedVolumePath;}
}
static void TestLogs(void) {
    UINT8 invalid[]={1,1,3,0},multi[sizeof(FixedVolumePath)+sizeof(UsbVolumePath)];
    UINT8 *bad_paths[]={NULL,invalid,multi};unsigned fixed,i;
    memcpy(multi,FixedVolumePath,sizeof(FixedVolumePath));multi[sizeof(FixedVolumePath)-3]=1;
    memcpy(multi+sizeof(FixedVolumePath),UsbVolumePath,sizeof(UsbVolumePath));
    for(fixed=0;fixed<2;fixed++) {
        SetupLog(fixed);CHECK(UefiMain(Image,&St)==EFI_SUCCESS);
        CHECK(FsRequests==1&&VolumeOpens==1&&PciEnumerations==1&&FileWrites==1&&CreatedSlot==1&&LogCloses==1&&ForbiddenCalls==0);
        SetupLog(fixed);FailedLog=TRUE;CHECK(UefiMain(Image,&St)==EFI_WRITE_PROTECTED);
        CHECK(PciEnumerations==0&&ConfigReads==0&&FileWrites==0&&ForbiddenCalls==0);
        SetupLog(fixed);MissingSourceFs=TRUE;CHECK(UefiMain(Image,&St)==EFI_NOT_FOUND);
        CHECK(FsRequests==1&&VolumeOpens==0&&PciEnumerations==0&&ConfigReads==0&&CreatedSlot==0&&FileWrites==0&&ForbiddenCalls==0);
        SetupLog(fixed);FailedWrite=TRUE;CHECK(UefiMain(Image,&St)==EFI_DEVICE_ERROR);CHECK(LogCloses==1&&ForbiddenCalls==0);
        SetupLog(fixed);ShortWrite=TRUE;CHECK(UefiMain(Image,&St)==EFI_DEVICE_ERROR);CHECK(LogCloses==1&&ForbiddenCalls==0);
        SetupLog(fixed);FailedFlush=TRUE;CHECK(UefiMain(Image,&St)==EFI_DEVICE_ERROR);CHECK(LogCloses==1&&ForbiddenCalls==0);
        SetupLog(fixed);ExistingFirst=TRUE;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CHECK(ExistingOpens==2&&ExistingCloses==1&&CreatedSlot==2&&ForbiddenCalls==0);
        SetupLog(fixed);ExistingAll=TRUE;CHECK(UefiMain(Image,&St)==EFI_VOLUME_FULL);
        CHECK(ExistingOpens==9999&&ExistingCloses==9999&&CreatedSlot==0&&FileWrites==0&&PciEnumerations==0&&ConfigReads==0&&ForbiddenCalls==0);
    }
    for(i=0;i<sizeof(bad_paths)/sizeof(bad_paths[0]);i++) {
        SetupLog(1);SourcePath=bad_paths[i];CHECK(UefiMain(Image,&St)==EFI_ACCESS_DENIED);
        CHECK(FsRequests==0&&VolumeOpens==0&&PciEnumerations==0&&ConfigReads==0&&CreatedSlot==0&&FileWrites==0&&ForbiddenCalls==0);
    }
}
static void TestCachedRom(void) {
    unsigned scenario;
    Setup(0);Rom[0x50]=1;MockPci[0].RomSize=512;CHECK(UefiMain(Image,&St)==EFI_SUCCESS);
    CHECK(!strstr(Written,"\"cached_rom_hex\":null")&&ForbiddenCalls==0);SaveCapture("build/host-capture-min-rom.json");
    Setup(0);Rom[0x50]=0;Rom[0x51]=8;MockPci[0].RomSize=TEST_ROM_MAX;
    CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CHECK(!strstr(Written,"\"cached_rom_hex\":null")&&ForbiddenCalls==0);
    SaveCapture("build/host-capture-max-rom.json");
    for(scenario=0;scenario<8;scenario++) {
        Setup(0);
        if(scenario==0) MockPci[0].RomImage=NULL;
        if(scenario==1) {MockPci[0].RomSize=TEST_ROM_MAX+1;MockPci[0].RomImage=(VOID *)(UINTN)1;}
        if(scenario==2) MockPci[0].RomSize=511;
        if(scenario==3) Rom[0]=0;
        if(scenario==4) Rom[0x46]=0x82;
        if(scenario==5) {Rom[0x4a]=0xff;Rom[0x4b]=0xff;}
        if(scenario==6) Rom[0x55]=0;
        if(scenario==7) MockPci[0].RomSize=0;
        CHECK(UefiMain(Image,&St)==EFI_SUCCESS);CHECK(strstr(Written,"\"cached_rom_hex\":null")&&strstr(Written,"\"sha256\":null"));
        CHECK(ForbiddenCalls==0);
    }
}
static void TestOverflow(void) {
    Setup(8);CHECK(UefiMain(Image,&St)==EFI_BUFFER_TOO_SMALL);
    CHECK(GraphReads==1&&ProtocolReads==1100&&FileWrites==0&&LogCloses==1&&ForbiddenCalls==0);
}
static void TestSamplers(void) {
    static CHAR8 buffer[3*1024*1024];TEXT t;UINTN reads;unsigned sample,mode,i;
    static const unsigned fields[]={0,2,8,11,14,44,46};
    static const UINT32 chips[]={0,0xffffffffu,0xbadf0001u,0x194000a2u,0x192000a1u};
    Setup(0);TextInit(&t,buffer,sizeof(buffer));reads=SampleRegisters(&t,&MockPci[0],&Bs);
    CHECK(reads==23&&MemoryReads==23&&RegisterReads==23&&PromReads==0&&ChipReads==2);
    CHECK(!t.failed&&strstr(buffer,"\"complete\":true")&&strstr(buffer,"\"pci_snapshot_unchanged\":true")&&ForbiddenCalls==0);
    for(i=0;i<22;i++) CHECK(RegisterSeen[i]==(i==0?2u:1u));
    Setup(0);TextInit(&t,buffer,sizeof(buffer));reads=SampleProm(&t,&MockPci[0],&Bs);
    CHECK(reads==259&&MemoryReads==259&&RegisterReads==2&&PromReads==257&&ChipReads==2);
    CHECK(!t.failed&&strstr(buffer,"\"complete\":true")&&strstr(buffer,"\"identity_stable\":true")&&ForbiddenCalls==0);
    CHECK(strstr(buffer,"\"length\":1048576")&&strstr(buffer,"\"hex\":\"55aa0000")&&strstr(buffer,"\"sha256\":\""));
    for(i=0;i<256;i++) CHECK(PromPages[i]==1);
    for(sample=0;sample<2;sample++) for(mode=0;mode<33;mode++) {
        EFI_PCI_IO_PROTOCOL *pci;EFI_BOOT_SERVICES *bs;
        Setup(0);pci=&MockPci[0];bs=&Bs;
        if(mode==0) pci=NULL;
        if(mode==1) bs=NULL;
        if(mode==2) MockPci[0].Pci.Read=NULL;
        if(mode==3) MockPci[0].Mem.Read=NULL;
        if(mode==4) MockPci[0].GetLocation=NULL;
        if(mode==5) MockPci[0].GetBarAttributes=NULL;
        if(mode==6) Bs.FreePool=NULL;
        if(mode==7) FailedConfig=TRUE;
        if(mode==8) FailedLocation=TRUE;
        if(mode>=9&&mode<16) Config[0][fields[mode-9]]^=1;
        if(mode==16) Config[0][4]=1;
        if(mode==17) Config[0][4]=7;
        if(mode==18) Config[0][19]=0;
        if(mode==19) Config[0][19]=0x90;
        if(mode==20) memset(Config[0]+16,0xff,4);
        if(mode>=21&&mode<29) ResourceFault=mode-20;
        if(mode>=29) LocationFault=mode-28;
        TextInit(&t,buffer,sizeof(buffer));
        reads=sample?SampleProm(&t,pci,bs):SampleRegisters(&t,pci,bs);
        CHECK(reads==0&&MemoryReads==0&&!t.failed&&ForbiddenCalls==0);
        CHECK(strstr(buffer,"\"guard_passed\":false")&&strstr(buffer,"\"read_attempts\":0")&&strstr(buffer,"\"complete\":false"));
        CHECK(!strstr(buffer,"\"hex\":\"")&&!strstr(buffer,"\"sha256\":\"")&&!strstr(buffer,"\"value\":\""));
    }
    for(sample=0;sample<2;sample++) for(i=0;i<sizeof(chips)/sizeof(chips[0]);i++) {
        Setup(0);ChipValue=chips[i];TextInit(&t,buffer,sizeof(buffer));
        reads=sample?SampleProm(&t,&MockPci[0],&Bs):SampleRegisters(&t,&MockPci[0],&Bs);
        CHECK(reads==1&&MemoryReads==1&&ChipReads==1&&PromReads==0&&ForbiddenCalls==0);
        CHECK(!t.failed&&strstr(buffer,"\"complete\":false"));
    }
    for(mode=0;mode<8;mode++) {
        Setup(0);
        if(mode==0) FailMemoryAt=1;
        if(mode==1) FailMemoryAt=2;
        if(mode==2) FailMemoryAt=23;
        if(mode==3) SentinelAt=1;
        if(mode==4) SentinelAt=2;
        if(mode==5) ChangeConfigAt=1;
        if(mode==6) FailConfigAfterMemory=22;
        if(mode==7) ChangeChipAt=2;
        TextInit(&t,buffer,sizeof(buffer));reads=SampleRegisters(&t,&MockPci[0],&Bs);
        CHECK(reads==MemoryReads&&reads>0&&reads<=23&&!t.failed&&ForbiddenCalls==0);
        if(mode==0||mode==3) CHECK(reads==1);
        if(mode==1||mode==4) CHECK(reads==2);
        if(mode==5||mode==6) CHECK(ChipReads==1);
        CHECK(strstr(buffer,"\"complete\":false"));
    }
    for(mode=0;mode<8;mode++) {
        Setup(0);
        if(mode==0) MockProm[0]=0;
        if(mode==1) FailMemoryAt=2;
        if(mode==2) FailProm=TRUE;
        if(mode==3) ChangePromSignature=TRUE;
        if(mode==4) ChangeConfigAt=3;
        if(mode==5) FailConfigAfterMemory=258;
        if(mode==6) FailMemoryAt=259;
        if(mode==7) ChangeChipAt=2;
        TextInit(&t,buffer,sizeof(buffer));reads=SampleProm(&t,&MockPci[0],&Bs);
        CHECK(reads==MemoryReads&&reads>=2&&reads<=259&&!t.failed&&ForbiddenCalls==0);
        if(mode<2) CHECK(reads==2);
        if(mode==2) CHECK(reads==4);
        CHECK(strstr(buffer,"\"complete\":false")&&!strstr(buffer,"\"hex\":\"")&&!strstr(buffer,"\"sha256\":\""));
    }
}
int main(int argc,char **argv) {
    if(argc==1||strcmp(argv[1],"core")==0) TestCore();
    if(argc==1||strcmp(argv[1],"identity")==0) TestIdentity();
    if(argc==1||strcmp(argv[1],"rom-parser")==0) TestRomParser();
    if(argc==1||strcmp(argv[1],"samplers")==0) TestSamplers();
    if(argc==1||strcmp(argv[1],"full-config")==0) TestFullConfig();
    if(argc==1||strcmp(argv[1],"application")==0) TestApplication();
    if(argc==1||strcmp(argv[1],"partial")==0) TestPartialApplication();
    if(argc==1||strcmp(argv[1],"logs")==0) TestLogs();
    if(argc==1||strcmp(argv[1],"cached-rom")==0) TestCachedRom();
    if(argc==1||strcmp(argv[1],"overflow")==0) TestOverflow();
    printf("PASS: %u checks; full EFI application executed against host mocks; no GPU hardware accessed.\n",Checks);return 0;
}
