/* SPDX-License-Identifier: MIT
 * Read-only ownership graph and loaded-driver code capture. This never stops
 * a driver, invokes DriverBinding callbacks, opens exclusively or changes TPL.
 * The graph is an observation, not an ownership permit or isolation proof.
 */
#include "platform.h"
#include <Protocol/DevicePath.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#define MAX_HANDLES 4096u
#define MAX_EDGES 4096u
#define MAX_AGENTS 32u
#define CODE_BUDGET (1024u*1024u)
static EFI_GUID PathId=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID DriverId=EFI_DRIVER_BINDING_PROTOCOL_GUID;
static EFI_GUID ImageId=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_HANDLE Agents[MAX_AGENTS];

static BOOLEAN Range(UINTN n,UINTN at,UINTN size) {return at<=n&&size<=n-at;}
BOOLEAN LoadedCodeView(const UINT8 *p,UINTN n,CODE_VIEW *out) {
    UINTN pe,opt,table,i,code_count=0;UINT32 reloc,reloc_bytes,at=0;CODE_VIEW v={0,0,0};
    if(!out)return FALSE;*out=v;
    if(!p||n>16u*1024u*1024u||!Range(n,0,64)||Read16(p)!=0x5a4d)return FALSE;
    pe=Read32(p+60);
    if(!Range(n,pe,24)||Read32(p+pe)!=0x4550||Read16(p+pe+4)!=0x8664||
       !Read16(p+pe+6)||Read16(p+pe+6)>32)return FALSE;
    opt=pe+24;
    if(Read16(p+pe+20)<240||!Range(n,opt,Read16(p+pe+20))||Read16(p+opt)!=0x20b)return FALSE;
    if(Read32(p+opt+56)>n||Read32(p+opt+108)<6)return FALSE;
    table=opt+Read16(p+pe+20);
    if(!Range(n,table,(UINTN)Read16(p+pe+6)*40))return FALSE;
    for(i=0;i<Read16(p+pe+6);i++) {
        const UINT8 *s=p+table+i*40;
        if(Read32(s+36)&0x20000000u) {
            if(++code_count!=1)return FALSE;
            v.rva=Read32(s+12);v.bytes=Read32(s+8);
            if(!v.bytes||v.bytes>512u*1024u||!Range(n,v.rva,v.bytes))return FALSE;
        }
    }
    if(code_count!=1)return FALSE;
    reloc=Read32(p+opt+112+5*8);reloc_bytes=Read32(p+opt+116+5*8);
    if((!reloc)!=(!reloc_bytes)||!Range(n,reloc,reloc_bytes))return FALSE;
    while(at<reloc_bytes) {
        UINT32 page,block;UINTN j;
        if(reloc_bytes-at<8)return FALSE;
        page=Read32(p+reloc+at);block=Read32(p+reloc+at+4);
        if(block<8||(block&1)||block>reloc_bytes-at)return FALSE;
        for(j=8;j<block;j+=2) {
            UINT16 entry=Read16(p+reloc+at+j);UINT64 pos=(UINT64)page+(entry&0xfff);
            if((entry>>12)&&pos>=(UINT64)v.rva&&pos<(UINT64)v.rva+v.bytes)v.relocations++;
        }
        at+=block;
    }
    *out=v;return TRUE;
}
static UINTN Index(EFI_HANDLE *h,UINTN n,EFI_HANDLE value) {
    UINTN i;for(i=0;i<n;i++)if(h[i]==value)return i;return MAX_UINTN;
}
static void HandleId(TEXT *t,EFI_HANDLE *h,UINTN n,EFI_HANDLE value) {
    UINTN id=Index(h,n,value);if(id==MAX_UINTN)TextPut(t,"null");else TextDec(t,id);
}
static UINT8 *Path(EFI_BOOT_SERVICES *bs,EFI_HANDLE h,UINTN *n) {
    VOID *p=NULL;BOOLEAN multi=FALSE;*n=0;
    if(!h||EFI_ERROR(bs->HandleProtocol(h,&PathId,&p))||!p||!DpMeasure(p,PROBE_DP_MAX,n,&multi)||multi)return NULL;
    return p;
}
static void Driver(TEXT *t,EFI_BOOT_SERVICES *bs,EFI_HANDLE agent,EFI_HANDLE *handles,UINTN count,UINTN *budget) {
    EFI_DRIVER_BINDING_PROTOCOL *binding=NULL;EFI_LOADED_IMAGE_PROTOCOL *image=NULL;
    EFI_STATUS s;CODE_VIEW view;UINT8 hash[32];UINTN path_n=0;UINT8 *path=NULL;
    TextPut(t,"{\"agent\":");HandleId(t,handles,count,agent);
    s=bs->HandleProtocol(agent,&DriverId,(VOID **)&binding);
    TextPut(t,",\"binding_status\":");TextHex(t,s,16);
    if(EFI_ERROR(s)||!binding){TextPut(t,"}");return;}
    TextPut(t,",\"version\":");TextHex(t,binding->Version,8);
    TextPut(t,",\"binding_handle_matches\":");TextPut(t,binding->DriverBindingHandle==agent?"true":"false");
    TextPut(t,",\"image_handle\":");HandleId(t,handles,count,binding->ImageHandle);
    s=bs->HandleProtocol(binding->ImageHandle,&ImageId,(VOID **)&image);
    TextPut(t,",\"image_status\":");TextHex(t,s,16);
    if(EFI_ERROR(s)||!image||!image->ImageBase||image->ImageSize>16u*1024u*1024u){TextPut(t,"}");return;}
    TextPut(t,",\"image_bytes\":");TextDec(t,image->ImageSize);
    TextPut(t,",\"code_type\":");TextDec(t,image->ImageCodeType);
    TextPut(t,",\"source_handle\":");HandleId(t,handles,count,image->DeviceHandle);
    TextPut(t,",\"stop_rva\":");
    if((UINTN)binding->Stop>=(UINTN)image->ImageBase&&(UINTN)binding->Stop-(UINTN)image->ImageBase<image->ImageSize)
        TextHex(t,(UINTN)binding->Stop-(UINTN)image->ImageBase,16);
    else TextPut(t,"null");
    TextPut(t,",\"file_path\":");
    if(image->FilePath) {BOOLEAN multi=FALSE;if(DpMeasure((UINT8 *)image->FilePath,PROBE_DP_MAX,&path_n,&multi)&&!multi)path=(UINT8 *)image->FilePath;}
    if(path)TextBytes(t,path,path_n);else TextPut(t,"null");
    TextPut(t,",\"code\":");
    if(!LoadedCodeView(image->ImageBase,(UINTN)image->ImageSize,&view)){TextPut(t,"null}");return;}
    TextPut(t,"{\"rva\":");TextHex(t,view.rva,8);TextPut(t,",\"length\":");TextDec(t,view.bytes);
    TextPut(t,",\"relocations_in_code\":");TextDec(t,view.relocations);
    Sha256((UINT8 *)image->ImageBase+view.rva,view.bytes,hash);
    TextPut(t,",\"loaded_sha256\":");TextBytes(t,hash,32);TextPut(t,",\"hex\":");
    if(view.bytes<=*budget){TextBytes(t,(UINT8 *)image->ImageBase+view.rva,view.bytes);*budget-=view.bytes;}
    else TextPut(t,"null");TextPut(t,"}}");
}
void SamplePlatform(TEXT *t,EFI_SYSTEM_TABLE *st,EFI_HANDLE target) {
    EFI_BOOT_SERVICES *bs=st?st->BootServices:NULL;EFI_HANDLE *handles=NULL;UINTN count=0,i,edges=0,agents=0,budget=CODE_BUDGET;
    UINTN target_n=0;UINT8 *target_path;BOOLEAN complete=TRUE,first=TRUE;EFI_STATUS status;
    if(!bs||!target||!bs->LocateHandleBuffer||!bs->HandleProtocol||!bs->FreePool||
       !bs->ProtocolsPerHandle||!bs->OpenProtocolInformation) {TextPut(t,"{\"available\":false,\"complete\":false}");return;}
    target_path=Path(bs,target,&target_n);
    status=bs->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&handles);
    TextPut(t,"{\"available\":true,\"enumeration_status\":");TextHex(t,status,16);
    TextPut(t,",\"handle_count\":");TextDec(t,count);
    if(EFI_ERROR(status)||!handles||!count||count>MAX_HANDLES||!target_path) {
        if(handles)bs->FreePool(handles);TextPut(t,",\"complete\":false}");return;
    }
    TextPut(t,",\"target\":");HandleId(t,handles,count,target);
    TextPut(t,",\"conout\":");HandleId(t,handles,count,st->ConsoleOutHandle);
    TextPut(t,",\"conin\":");HandleId(t,handles,count,st->ConsoleInHandle);
    TextPut(t,",\"stderr\":");HandleId(t,handles,count,st->StandardErrorHandle);
    TextPut(t,",\"handles\":[");
    for(i=0;i<count;i++) {
        UINTN len=0,n=0,j;UINT8 *path=Path(bs,handles[i],&len);EFI_GUID **protocols=NULL;
        BOOLEAN related=(BOOLEAN)(handles[i]==target||(path&&DpParent(target_path,target_n,path,len)));
        if(i)TextPut(t,",");TextPut(t,"{\"id\":");TextDec(t,i);TextPut(t,",\"gpu_descendant\":");TextPut(t,related?"true":"false");
        TextPut(t,",\"path\":");if(path)TextBytes(t,path,len);else TextPut(t,"null");
        status=bs->ProtocolsPerHandle(handles[i],&protocols,&n);
        TextPut(t,",\"protocol_status\":");TextHex(t,status,16);TextPut(t,",\"active_opens\":[");first=TRUE;
        if(EFI_ERROR(status)||!protocols||n>64){complete=FALSE;n=0;}
        for(j=0;j<n;j++) {
            EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *info=NULL;UINTN entries=0,k;
            if(!protocols[j]){complete=FALSE;continue;}
            status=bs->OpenProtocolInformation(handles[i],protocols[j],&info,&entries);
            if(EFI_ERROR(status)||(entries&&!info)||entries>128){complete=FALSE;entries=0;}
            for(k=0;k<entries;k++) {
                UINT32 attr=info[k].Attributes;
                if(attr==EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL||attr==EFI_OPEN_PROTOCOL_GET_PROTOCOL||attr==EFI_OPEN_PROTOCOL_TEST_PROTOCOL)continue;
                if(edges>=MAX_EDGES){complete=FALSE;break;}edges++;
                if(!first)TextPut(t,",");first=FALSE;
                TextPut(t,"{\"protocol\":");TextBytes(t,(UINT8 *)protocols[j],16);
                TextPut(t,",\"agent\":");HandleId(t,handles,count,info[k].AgentHandle);
                TextPut(t,",\"controller\":");HandleId(t,handles,count,info[k].ControllerHandle);
                TextPut(t,",\"attributes\":");TextHex(t,attr,8);TextPut(t,",\"count\":");TextDec(t,info[k].OpenCount);TextPut(t,"}");
                if(related&&info[k].AgentHandle&&Index(Agents,agents,info[k].AgentHandle)==MAX_UINTN) {
                    if(agents<MAX_AGENTS)Agents[agents++]=info[k].AgentHandle;else complete=FALSE;
                }
            }
            if(info&&EFI_ERROR(bs->FreePool(info)))complete=FALSE;
        }
        if(protocols&&EFI_ERROR(bs->FreePool(protocols)))complete=FALSE;
        TextPut(t,"]}");
    }
    TextPut(t,"],\"gpu_agents\":[");
    for(i=0;i<agents;i++){if(i)TextPut(t,",");Driver(t,bs,Agents[i],handles,count,&budget);}
    TextPut(t,"],\"edge_count\":");TextDec(t,edges);
    TextPut(t,",\"complete\":");TextPut(t,complete?"true":"false");
    TextPut(t,",\"atomic_snapshot\":false,\"release_authorized\":false}");
    bs->FreePool(handles);
}
