/* SPDX-License-Identifier: MIT */
#include "../src/platform.h"
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Mode;
#define CHECK(x) do {Checks++;if(!(x)){fprintf(stderr,"platform FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static UINT8 Pe[4096],Parent[]={1,1,6,0,0,1,0x7f,0xff,4,0};
static UINT8 Child[]={1,1,6,0,0,1,2,3,8,0,0,0,0,0,0x7f,0xff,4,0};
static EFI_GUID DriverGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID,LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID,DummyGuid={1,2,3,{4}};
static EFI_DRIVER_BINDING_PROTOCOL Binding;
static EFI_LOADED_IMAGE_PROTOCOL Image;
static EFI_HANDLE H(UINTN n){return (EFI_HANDLE)n;}
static VOID Put16(UINTN at,UINT16 v){Pe[at]=(UINT8)v;Pe[at+1]=(UINT8)(v>>8);}
static VOID Put32(UINTN at,UINT32 v){for(unsigned i=0;i<4;i++)Pe[at+i]=(UINT8)(v>>(8*i));}
static VOID SetupPe(VOID) {
    memset(Pe,0,sizeof(Pe));Put16(0,0x5a4d);Put32(60,64);Put32(64,0x4550);Put16(68,0x8664);Put16(70,1);Put16(84,240);
    Put16(88,0x20b);Put32(88+56,4096);Put32(88+108,6);
    Put32(328+8,16);Put32(328+12,0x200);Put32(328+36,0x20000000);
    for(unsigned i=0;i<16;i++)Pe[0x200+i]=(UINT8)i;
}
static EFI_STATUS EFIAPI Free(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE kind,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **out) {
    CHECK(kind==AllHandles&&!guid&&!key);*n=Mode==1?4097:4;
    *out=malloc(4*sizeof(EFI_HANDLE));CHECK(*out);
    for(UINTN i=0;i<4;i++)(*out)[i]=H(i+1);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *guid,VOID **out) {
    *out=NULL;
    if(BytesEqual(guid,&PathGuid,16)) {
        if(h==H(1))*out=Parent;else if(h==H(2))*out=Child;
    } else if(h==H(3)&&BytesEqual(guid,&DriverGuid,16))*out=&Binding;
    else if(h==H(3)&&BytesEqual(guid,&LoadedGuid,16))*out=&Image;
    return *out?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Protocols(EFI_HANDLE h,EFI_GUID ***out,UINTN *n) {
    (void)h;*n=Mode==2?65:1;*out=malloc(sizeof(EFI_GUID *));CHECK(*out);(*out)[0]=&DummyGuid;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Opens(EFI_HANDLE h,EFI_GUID *guid,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **out,UINTN *n) {
    CHECK(BytesEqual(guid,&DummyGuid,16));*n=0;*out=NULL;
    if(Mode==3)return EFI_DEVICE_ERROR;
    if(h==H(1)||h==H(2)) {
        *n=h==H(1)?2:1;*out=calloc(*n,sizeof(**out));CHECK(*out);
        (*out)[0].AgentHandle=H(3);(*out)[0].ControllerHandle=h;
        (*out)[0].Attributes=EFI_OPEN_PROTOCOL_BY_DRIVER;(*out)[0].OpenCount=1;
        if(*n==2){(*out)[1]=(*out)[0];(*out)[1].ControllerHandle=H(2);(*out)[1].Attributes=EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER;}
    }
    return EFI_SUCCESS;
}
static void TestPe(void) {
    CODE_VIEW v;SetupPe();CHECK(LoadedCodeView(Pe,sizeof(Pe),&v)&&v.rva==0x200&&v.bytes==16&&!v.relocations);
    Put32(88+112+5*8,0x300);Put32(88+116+5*8,12);Put32(0x300,0);Put32(0x304,12);Put16(0x308,0xa208);
    CHECK(LoadedCodeView(Pe,sizeof(Pe),&v)&&v.relocations==1);
    Put32(0x304,0);CHECK(!LoadedCodeView(Pe,sizeof(Pe),&v));
    SetupPe();Put32(328+12,0xfffffff0);CHECK(!LoadedCodeView(Pe,sizeof(Pe),&v));
    SetupPe();Put16(70,2);Put32(368+36,0x20000000);CHECK(!LoadedCodeView(Pe,sizeof(Pe),&v));
    SetupPe();for(UINTN n=0;n<4096;n+=31)CHECK(!LoadedCodeView(Pe,n,&v));
}
static void TestGraph(void) {
    static CHAR8 data[65536];TEXT t;EFI_BOOT_SERVICES bs;EFI_SYSTEM_TABLE st;
    memset(&bs,0,sizeof(bs));memset(&st,0,sizeof(st));st.BootServices=&bs;
    TextInit(&t,data,sizeof(data));SamplePlatform(&t,&st,H(1));CHECK(strstr(data,"\"available\":false"));
    SetupPe();memset(&Binding,0,sizeof(Binding));memset(&Image,0,sizeof(Image));
    Binding.DriverBindingHandle=Binding.ImageHandle=H(3);Binding.Version=0x6000b;
    Binding.Stop=(EFI_DRIVER_BINDING_STOP)(UINTN)(Pe+0x208);Image.ImageBase=Pe;Image.ImageSize=sizeof(Pe);
    Image.ImageCodeType=EfiBootServicesCode;Image.DeviceHandle=H(1);Image.FilePath=(EFI_DEVICE_PATH_PROTOCOL *)Parent;
    bs.LocateHandleBuffer=Locate;bs.HandleProtocol=Handle;bs.FreePool=Free;bs.ProtocolsPerHandle=Protocols;bs.OpenProtocolInformation=Opens;
    for(Mode=0;Mode<4;Mode++) {
        TextInit(&t,data,sizeof(data));SamplePlatform(&t,&st,H(1));CHECK(!t.failed);
        if(!Mode) {
            CHECK(strstr(data,"\"complete\":true")&&strstr(data,"\"edge_count\":3"));
            CHECK(strstr(data,"\"stop_rva\":\"0x0000000000000208\"")&&strstr(data,"\"hex\":\"000102030405060708090a0b0c0d0e0f\""));
            CHECK(strstr(data,"\"release_authorized\":false"));
        } else CHECK(strstr(data,"\"complete\":false"));
    }
}
int main(void){TestPe();TestGraph();printf("PASS: %u platform PE/ownership checks; read-only host mocks.\n",Checks);return 0;}
