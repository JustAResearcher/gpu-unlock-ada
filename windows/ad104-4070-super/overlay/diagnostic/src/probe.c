/* SPDX-License-Identifier: MIT
 * Read-only cold diagnostic. Fixed guarded register/PROM reads, PCI config,
 * cached ROM and firmware-owned metadata. Firmware console output remains.
 * The only persistent writes are new log files on the application's source volume.
 */
#include "core.h"
#include "sample.h"
#include "platform.h"
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/GraphicsOutput.h>
#ifndef PROBE_BUILD_ID
#error Build with Build.ps1 to bind captures to the source manifest
#endif

#define MAX_DISPLAYS 32
#define MAX_PCI_HANDLES 512
#define MAX_GOP_HANDLES 64
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;
static EFI_GUID DpGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID LoadedGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0x00,0xe0,0x98,0x03,0x2b,0x8c}};
static CHAR8 Report[8*1024*1024];
/* Retain an absolute address fixup for relocatable PE/COFF firmware loading. */
static CHAR8 * volatile RelocationAnchor=Report;
typedef struct {
    EFI_HANDLE handle;
    EFI_PCI_IO_PROTOCOL *pci;
    UINT8 config[64];
    UINT8 *path;
    UINTN path_length,segment,bus,device,function;
    BOOLEAN multi,location_valid;
} DISPLAY;
static DISPLAY Displays[MAX_DISPLAYS];
static UINTN DisplayCount,TargetCount,ExactCount;
static BOOLEAN EnumerationComplete;

static void Status(TEXT *t,EFI_STATUS s) {TextHex(t,(UINT64)s,16);}
static void Bool(TEXT *t,BOOLEAN b) {TextPut(t,b?"true":"false");}
static void Message(EFI_SYSTEM_TABLE *st,CHAR16 *s) {
    if(st->ConOut && st->ConOut->OutputString) st->ConOut->OutputString(st->ConOut,s);
}
static UINT8 *PathFor(EFI_BOOT_SERVICES *bs,EFI_HANDLE h,UINTN *len,BOOLEAN *multi) {
    VOID *p=NULL; EFI_STATUS s; *len=0;*multi=FALSE;
    if(!h) return NULL;
    s=bs->HandleProtocol(h,&DpGuid,&p);
    if(EFI_ERROR(s)||!p||!DpMeasure(p,PROBE_DP_MAX,len,multi)) return NULL;
    return p;
}
static void PathJson(TEXT *t,UINT8 *path,UINTN len,BOOLEAN multi) {
    TextPut(t,"{\"valid\":");Bool(t,(BOOLEAN)(path!=NULL));
    TextPut(t,",\"multi_instance\":");Bool(t,multi);
    TextPut(t,",\"hex\":");if(path) TextBytes(t,path,len);else TextPut(t,"null");TextPut(t,"}");
}
static void VariableByte(TEXT *t,EFI_SYSTEM_TABLE *st,CHAR16 *name) {
    UINT8 value=0; UINTN size=1; UINT32 attributes=0;
    EFI_STATUS s=st->RuntimeServices->GetVariable(name,&GlobalGuid,&attributes,&size,&value);
    TextPut(t,"{\"status\":");Status(t,s);TextPut(t,",\"value\":");
    if(!EFI_ERROR(s)&&size==1) TextDec(t,value);else TextPut(t,"null");TextPut(t,"}");
}
static void VariablePath(TEXT *t,EFI_SYSTEM_TABLE *st,CHAR16 *name) {
    UINT8 buffer[PROBE_DP_MAX];UINTN size=sizeof(buffer),length=0;UINT32 attributes=0;BOOLEAN multi=FALSE;
    EFI_STATUS s=st->RuntimeServices->GetVariable(name,&GlobalGuid,&attributes,&size,buffer);
    TextPut(t,"{\"status\":");Status(t,s);TextPut(t,",\"path\":");
    if(!EFI_ERROR(s)&&size<=sizeof(buffer)&&DpMeasure(buffer,size,&length,&multi)&&length==size)
        PathJson(t,buffer,length,multi);
    else TextPut(t,"null");TextPut(t,"}");
}
static void BarJson(TEXT *t,DISPLAY *d,UINTN index,EFI_BOOT_SERVICES *bs) {
    UINT32 raw=Read32(d->config+16+index*4);UINT64 base,supports=0;VOID *resources=NULL;EFI_STATUS s;
    BOOLEAN is_io=(BOOLEAN)((raw&1)!=0),is64=(BOOLEAN)(!is_io&&(raw&6)==4);
    TextPut(t,"{\"index\":");TextDec(t,index);TextPut(t,",\"raw\":");TextHex(t,raw,8);
    TextPut(t,",\"io\":");Bool(t,is_io);TextPut(t,",\"width64\":");Bool(t,is64);
    base=is_io?(raw&~(UINT64)3):(raw&~(UINT64)15);
    if(is64&&index<5) base|=(UINT64)Read32(d->config+20+index*4)<<32;
    TextPut(t,",\"base\":");TextHex(t,base,16);
    s=d->pci->GetBarAttributes?d->pci->GetBarAttributes(d->pci,(UINT8)index,&supports,&resources):EFI_UNSUPPORTED;
    TextPut(t,",\"descriptor_status\":");Status(t,s);
    TextPut(t,",\"resource\":");
    if(!EFI_ERROR(s)&&resources) {
        const UINT8 *p=resources;
        if(p[0]==0x8a&&Read16(p+1)==43) {
            UINT64 resource_size=0;
            TextPut(t,"{\"matches_config\":");Bool(t,ResourceMatches(p,raw,base,&resource_size));
            TextPut(t,",\"type\":");TextDec(t,p[3]);
            TextPut(t,",\"minimum\":");TextHex(t,Read64(p+14),16);
            TextPut(t,",\"maximum\":");TextHex(t,Read64(p+22),16);
            TextPut(t,",\"translation\":");TextHex(t,Read64(p+30),16);
            TextPut(t,",\"length\":");TextHex(t,Read64(p+38),16);TextPut(t,"}");
        } else TextPut(t,"null");
    } else TextPut(t,"null");
    if(resources) bs->FreePool(resources);
    TextPut(t,"}");
}
static void DisplayJson(TEXT *t,DISPLAY *d,EFI_BOOT_SERVICES *bs) {
    UINTN i;UINT8 hash[32];BOOLEAN target=(BOOLEAN)(Read16(d->config)==0x10de&&Read16(d->config+2)==0x2783);
    TextPut(t,"{\"vendor\":");TextHex(t,Read16(d->config),4);
    TextPut(t,",\"device_id\":");TextHex(t,Read16(d->config+2),4);
    TextPut(t,",\"subsystem_vendor\":");TextHex(t,Read16(d->config+44),4);
    TextPut(t,",\"subsystem_device\":");TextHex(t,Read16(d->config+46),4);
    TextPut(t,",\"revision\":");TextDec(t,d->config[8]);
    TextPut(t,",\"exact_expected_identity\":");Bool(t,ExactTarget(d->config,64));
    TextPut(t,",\"location_valid\":");Bool(t,d->location_valid);
    TextPut(t,",\"location\":");
    if(d->location_valid) {
        TextPut(t,"{\"segment\":");TextDec(t,d->segment);TextPut(t,",\"bus\":");TextDec(t,d->bus);
        TextPut(t,",\"device\":");TextDec(t,d->device);TextPut(t,",\"function\":");TextDec(t,d->function);TextPut(t,"}");
    } else TextPut(t,"null");
    TextPut(t,",\"config64\":");TextBytes(t,d->config,64);
    TextPut(t,",\"path\":");PathJson(t,d->path,d->path_length,d->multi);
    TextPut(t,",\"bars\":[");
    if((d->config[14]&0x7f)==0) for(i=0;i<6;i++) {
        UINT32 raw=Read32(d->config+16+i*4);
        if(i) TextPut(t,",");BarJson(t,d,i,bs);
        if(!(raw&1)&&(raw&6)==4&&i<5) ++i;
    }
    TextPut(t,"],\"cached_efi_rom\":{\"size\":");TextDec(t,d->pci->RomSize);
    TextPut(t,",\"sha256\":");
    /* Hash only an exact expected card's bounded, structurally checked cached ROM. */
    if(target&&ExactTarget(d->config,64)&&d->pci->RomSize<=PROBE_ROM_MAX&&
        RomPcirValid(d->pci->RomImage,(UINTN)d->pci->RomSize,0x10de,0x2783)) {
        Sha256(d->pci->RomImage,(UINTN)d->pci->RomSize,hash);TextBytes(t,hash,32);
    } else TextPut(t,"null");
    TextPut(t,",\"source\":\"EFI_PCI_IO_PROTOCOL.RomImage; not a physical PROM dump\"}}");
}
static void EnumerateDisplays(EFI_BOOT_SERVICES *bs,TEXT *t) {
    EFI_HANDLE *handles=NULL;UINTN count=0,i;EFI_STATUS s;BOOLEAN first=TRUE;
    DisplayCount=0;TargetCount=0;ExactCount=0;EnumerationComplete=TRUE;
    s=bs->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,&count,&handles);
    TextPut(t,"\"pci_enumeration_status\":");Status(t,s);TextPut(t,",\"display_devices\":[");
    if(EFI_ERROR(s)||count>MAX_PCI_HANDLES||(!handles&&count)) {EnumerationComplete=FALSE;goto done;}
    for(i=0;i<count;i++) {
        EFI_PCI_IO_PROTOCOL *pci=NULL;UINT32 words[16];UINT8 *config=(UINT8 *)words;DISPLAY *d;
        s=bs->HandleProtocol(handles[i],&PciGuid,(VOID **)&pci);
        if(EFI_ERROR(s)||!pci||!pci->Pci.Read) {EnumerationComplete=FALSE;continue;}
        s=pci->Pci.Read(pci,EfiPciIoWidthUint32,0,16,config);
        if(EFI_ERROR(s)) {EnumerationComplete=FALSE;continue;}
        if(config[11]!=3) continue;
        if(DisplayCount>=MAX_DISPLAYS) {EnumerationComplete=FALSE;continue;}
        d=&Displays[DisplayCount++];d->pci=pci;d->handle=handles[i];
        {UINTN k;for(k=0;k<64;k++) d->config[k]=config[k];}
        s=pci->GetLocation?pci->GetLocation(pci,&d->segment,&d->bus,&d->device,&d->function):EFI_UNSUPPORTED;
        d->location_valid=(BOOLEAN)(!EFI_ERROR(s)&&d->segment<=65535&&d->bus<=255&&d->device<32&&d->function<8);
        d->path=PathFor(bs,handles[i],&d->path_length,&d->multi);
        if(!d->location_valid||!d->path||d->multi) EnumerationComplete=FALSE;
        if(Read16(config)==0x10de&&Read16(config+2)==0x2783) ++TargetCount;
        if(ExactTarget(config,64)) ++ExactCount;
        if(!first) TextPut(t,",");first=FALSE;DisplayJson(t,d,bs);
    }
done:
    if(handles) bs->FreePool(handles);
    TextPut(t,"],\"pci_enumeration_complete\":");Bool(t,EnumerationComplete);
}
static INTN ParentIndex(UINT8 *path,UINTN length) {
    UINTN i,best_len=0;INTN best=-1;
    for(i=0;i<DisplayCount;i++) {
        DISPLAY *d=&Displays[i];
        if(!d->path||!DpParent(d->path,d->path_length,path,length)) continue;
        if(d->path_length>best_len) {best=(INTN)i;best_len=d->path_length;}
        else if(d->path_length==best_len) best=-2;
    }
    return best;
}
static void Gops(EFI_SYSTEM_TABLE *st,TEXT *t) {
    EFI_HANDLE *handles=NULL;UINTN count=0,i;EFI_STATUS s;BOOLEAN complete;
    s=st->BootServices->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&handles);
    complete=(BOOLEAN)(!EFI_ERROR(s)&&count<=MAX_GOP_HANDLES&&(!count||handles!=NULL));
    TextPut(t,",\"gop_enumeration_status\":");Status(t,s);TextPut(t,",\"gops\":[");
    if(!EFI_ERROR(s)&&handles&&count<=MAX_GOP_HANDLES) for(i=0;i<count;i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=NULL;UINTN len=0;BOOLEAN multi=FALSE;UINT8 *path;INTN parent;
        if(i) TextPut(t,",");
        path=PathFor(st->BootServices,handles[i],&len,&multi);parent=path?ParentIndex(path,len):-1;
        if(!path||multi||parent<0) complete=FALSE;
        TextPut(t,"{\"path\":");PathJson(t,path,len,multi);
        TextPut(t,",\"display_parent_index\":");if(parent>=0) TextDec(t,(UINT64)parent);else TextPut(t,"null");
        TextPut(t,",\"ambiguous_parent\":");Bool(t,(BOOLEAN)(parent==-2));
        TextPut(t,",\"same_handle_as_console_out\":");Bool(t,(BOOLEAN)(handles[i]==st->ConsoleOutHandle));
        s=st->BootServices->HandleProtocol(handles[i],&GopGuid,(VOID **)&gop);
        if(EFI_ERROR(s)||!gop) complete=FALSE;
        TextPut(t,",\"protocol_status\":");Status(t,s);
        TextPut(t,",\"current_mode\":");
        if(!EFI_ERROR(s)&&gop&&gop->Mode&&gop->Mode->Info&&gop->Mode->SizeOfInfo>=sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION)) {
            TextPut(t,"{\"width\":");TextDec(t,gop->Mode->Info->HorizontalResolution);
            TextPut(t,",\"height\":");TextDec(t,gop->Mode->Info->VerticalResolution);
            TextPut(t,",\"framebuffer_base\":");TextHex(t,gop->Mode->FrameBufferBase,16);
            TextPut(t,",\"framebuffer_size\":");TextHex(t,gop->Mode->FrameBufferSize,16);TextPut(t,"}");
        } else TextPut(t,"null");TextPut(t,"}");
    }
    TextPut(t,"],\"gop_count\":");TextDec(t,count);
    TextPut(t,",\"gop_enumeration_complete\":");Bool(t,complete);
    if(handles) st->BootServices->FreePool(handles);
}
static EFI_STATUS OpenLog(EFI_HANDLE image,EFI_SYSTEM_TABLE *st,EFI_FILE_PROTOCOL **log,CHAR16 filename[13]) {
    EFI_LOADED_IMAGE_PROTOCOL *loaded=NULL;EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs=NULL;
    EFI_FILE_PROTOCOL *root=NULL,*dir=NULL,*existing=NULL;EFI_STATUS s,close_status;UINTN slot,k,path_len=0;
    BOOLEAN multi=FALSE;UINT8 *volume_path;
    *log=NULL;
    s=st->BootServices->HandleProtocol(image,&LoadedGuid,(VOID **)&loaded);
    if(EFI_ERROR(s)||!loaded) return EFI_NOT_FOUND;
    volume_path=PathFor(st->BootServices,loaded->DeviceHandle,&path_len,&multi);
    if(!volume_path||multi) return EFI_ACCESS_DENIED;
    s=st->BootServices->HandleProtocol(loaded->DeviceHandle,&FsGuid,(VOID **)&fs);
    if(EFI_ERROR(s)||!fs) return EFI_NOT_FOUND;
    s=fs->OpenVolume(fs,&root);if(EFI_ERROR(s)||!root) return EFI_ERROR(s)?s:EFI_DEVICE_ERROR;
    s=root->Open(root,&dir,L"AD104-PROBE",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
    close_status=root->Close(root);if(!EFI_ERROR(s)&&EFI_ERROR(close_status)) s=close_status;
    if(EFI_ERROR(s)||!dir) {if(dir) dir->Close(dir);return EFI_ERROR(s)?s:EFI_DEVICE_ERROR;}
    for(slot=1;slot<=9999;slot++) {
        const CHAR16 template_name[]=L"CAP0000.JSON";UINTN number=slot;
        for(k=0;k<13;k++) filename[k]=template_name[k];
        for(k=7;k>3;) {--k;filename[k]=(CHAR16)(L'0'+number%10);number/=10;}
        existing=NULL;
        s=dir->Open(dir,&existing,filename,EFI_FILE_MODE_READ,0);
        if(!EFI_ERROR(s)) {if(existing) existing->Close(existing);continue;}
        if(s!=EFI_NOT_FOUND) break;
        s=dir->Open(dir,log,filename,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);break;
    }
    if(slot>9999) s=EFI_VOLUME_FULL;
    close_status=dir->Close(dir);if(!EFI_ERROR(s)&&EFI_ERROR(close_status)) s=close_status;
    if(EFI_ERROR(s)&&*log) {(*log)->Close(*log);*log=NULL;}
    return s;
}
static void ConsolePath(TEXT *t,EFI_SYSTEM_TABLE *st,EFI_HANDLE h) {
    UINTN length=0;BOOLEAN multi=FALSE;UINT8 *path=PathFor(st->BootServices,h,&length,&multi);
    PathJson(t,path,length,multi);
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    TEXT t;EFI_FILE_PROTOCOL *log=NULL;EFI_STATUS s,flush_status,close_status;UINTN size,mmio_attempts=0,index;CHAR16 filename[13];
    DISPLAY *target=NULL;
    if(!st||!st->BootServices||!st->RuntimeServices||RelocationAnchor!=Report) return EFI_INVALID_PARAMETER;
    Message(st,L"4070 SUPER read-only cold diagnostic - fixed MMIO/PROM reads; no unlock.\r\n");
    s=OpenLog(image,st,&log,filename);
    if(EFI_ERROR(s)||!log) {Message(st,L"Source volume unavailable, or log could not be opened. Returning to firmware.\r\n");return EFI_ERROR(s)?s:EFI_DEVICE_ERROR;}
    TextInit(&t,Report,sizeof(Report));
    TextPut(&t,"{\n\"schema\":5,\"probe_version\":\"" PROBE_VERSION "\",\"probe_mode\":\"readonly-cold-4070s\",\"build_id\":\"" PROBE_BUILD_ID "\",\"gpu_writes\":false,\"unlock_attempted\":false,");
    TextPut(&t,"\"hardware_support_validated\":false,\"uefi_revision\":");TextHex(&t,st->Hdr.Revision,8);
    TextPut(&t,",\"secure_boot\":");VariableByte(&t,st,L"SecureBoot");
    TextPut(&t,",\"setup_mode\":");VariableByte(&t,st,L"SetupMode");TextPut(&t,",");
    EnumerateDisplays(st->BootServices,&t);Gops(st,&t);
    TextPut(&t,",\"console_out_handle_path\":");ConsolePath(&t,st,st->ConsoleOutHandle);
    TextPut(&t,",\"console_in_handle_path\":");ConsolePath(&t,st,st->ConsoleInHandle);
    TextPut(&t,",\"conout_variable\":");VariablePath(&t,st,L"ConOut");
    TextPut(&t,",\"conin_variable\":");VariablePath(&t,st,L"ConIn");
    TextPut(&t,",\"target_id_count\":");TextDec(&t,TargetCount);
    TextPut(&t,",\"exact_identity_count\":");TextDec(&t,ExactCount);
    TextPut(&t,",\"unique_expected_identity_observed\":");Bool(&t,(BOOLEAN)(EnumerationComplete&&TargetCount==1&&ExactCount==1));
    if(EnumerationComplete&&TargetCount==1&&ExactCount==1)
        for(index=0;index<DisplayCount;index++) if(ExactTarget(Displays[index].config,64)) target=&Displays[index];
    TextPut(&t,",\"register_snapshot\":");
    if(target) mmio_attempts=SampleRegisters(&t,target->pci,st->BootServices);
    else TextPut(&t,"{\"guard_passed\":false,\"reason\":\"no_unique_target\",\"read_attempts\":0,\"complete\":false}");
    TextPut(&t,",\"physical_prom\":");
    if(target) mmio_attempts+=SampleProm(&t,target->pci,st->BootServices);
    else TextPut(&t,"{\"guard_passed\":false,\"reason\":\"no_unique_target\",\"read_attempts\":0,\"complete\":false}");
    {
        UINT32 extended[64],full[1024];BOOLEAN extended_valid=FALSE;
        TextPut(&t,",\"config256\":");
        if(target) {
            s=target->pci->Pci.Read(target->pci,EfiPciIoWidthUint32,0,64,extended);
            extended_valid=(BOOLEAN)(!EFI_ERROR(s)&&BytesEqual(extended,target->config,64));
        }
        if(extended_valid) TextBytes(&t,(UINT8 *)extended,sizeof(extended));else TextPut(&t,"null");
        TextPut(&t,",\"config4096\":");
        s=EFI_NOT_FOUND;
        if(target) {
            s=target->pci->Pci.Read(target->pci,EfiPciIoWidthUint32,0,1024,full);
            if(!EFI_ERROR(s)&&(!BytesEqual(full,target->config,64)||
                (extended_valid&&!BytesEqual(full,extended,sizeof(extended))))) s=EFI_COMPROMISED_DATA;
        }
        if(!EFI_ERROR(s)) TextBytes(&t,(UINT8 *)full,sizeof(full));else TextPut(&t,"null");
        TextPut(&t,",\"config4096_status\":");Status(&t,s);
    }
    TextPut(&t,",\"mmio_reads\":");Bool(&t,(BOOLEAN)(mmio_attempts>0));
    /* Keep the firmware-cached ROM distinct from the guarded physical PROM sample. */
    TextPut(&t,",\"cached_rom_hex\":");
    if(target&&target->pci->RomSize<=PROBE_ROM_MAX&&
        RomPcirValid(target->pci->RomImage,(UINTN)target->pci->RomSize,0x10de,0x2783))
        TextBytes(&t,target->pci->RomImage,(UINTN)target->pci->RomSize);
    else TextPut(&t,"null");
    TextPut(&t,",\"platform\":");SamplePlatform(&t,st,target?target->handle:NULL);
    TextPut(&t,",\"limitations\":[\"diagnostic reads do not establish unlock eligibility\",\"cached ROM is not physical PROM\",\"GOP inventory does not prove display isolation\"]}\n");
    if(t.failed) {log->Close(log);Message(st,L"Report buffer limit reached. Capture incomplete.\r\n");return EFI_BUFFER_TOO_SMALL;}
    size=t.len;s=log->Write(log,&size,Report);
    if(!EFI_ERROR(s)&&size!=t.len) s=EFI_DEVICE_ERROR;
    flush_status=log->Flush(log);close_status=log->Close(log);
    if(!EFI_ERROR(s)&&EFI_ERROR(flush_status)) s=flush_status;
    if(!EFI_ERROR(s)&&EFI_ERROR(close_status)) s=close_status;
    if(EFI_ERROR(s)) Message(st,L"Log write/flush failed. Do not treat this as a valid capture.\r\n");
    else {Message(st,L"Saved AD104-PROBE\\");Message(st,filename);Message(st,L"\r\nProbe finished. No unlock performed. Returning to firmware in 8 seconds.\r\n");}
    if(st->BootServices->Stall) st->BootServices->Stall(8000000);
    return s;
}
