/* SPDX-License-Identifier: MIT
 * Fixed read-only diagnostic register list. No register writes, polling, reset,
 * ownership changes, firmware execution or DMA mapping are provided here.
 * Addresses: gpu-unlock-ada 491925c diagnostics/peek and gfw_eval.h.
 */
#include "sample.h"

typedef struct {const CHAR8 *name;UINT32 offset;} REGISTER;
static const REGISTER Registers[]={
    {"PMC_BOOT_0",0x00000000},
    {"GFW_PLM",0x00118128},{"GFW_BOOT",0x00118234},{"GFW_CPUCTL",0x00110100},
    {"WPR2_LO",0x001fa824},{"WPR2_HI",0x001fa828},
    {"WPR_CFG_PLM",0x001fa7cc},{"WPR_PLM",0x001fa7c4},
    {"FBPA_PLM",0x009a0148},{"FEAT_PLM",0x00823804},
    {"FEAT_READOUT_0",0x00823814},{"FEAT_READOUT_1",0x00823818},
    {"SS0",0x0082381c},{"SS1",0x00823820},{"FB_SIZE_MB",0x001183a4},{"VGA_BASE",0x00625f04},{"MMU_PLM",0x001fa7c8},
    {"MMU_LO",0x001fa82c},{"MMU_HI",0x001fa830},{"BOOTER_UCODE_FUSE",0x00824148},
    {"FWSEC_UCODE_FUSE",0x008241e0},{"SEC2_CPUCTL",0x00840100}
};

BOOLEAN ResourceMatches(const UINT8 *p,UINT32 raw,UINT64 base,UINT64 *size) {
    UINT64 minimum,maximum,length;*size=0;
    if(!p||p[0]!=0x8a||Read16(p+1)!=43||raw==0||raw==0xffffffffu) return FALSE;
    if(p[3]!=(UINT8)((raw&1)?1:0)||Read64(p+30)!=0) return FALSE;
    minimum=Read64(p+14);maximum=Read64(p+22);length=Read64(p+38);
    if(!base||minimum!=base||maximum<minimum||maximum-minimum==MAX_UINT64||!length||maximum-minimum+1!=length) return FALSE;
    *size=length;return TRUE;
}
static void FailureCount(TEXT *t,const CHAR8 *reason,UINTN attempts) {
    TextPut(t,"{\"guard_passed\":false,\"reason\":\"");TextPut(t,reason);
    TextPut(t,"\",\"read_attempts\":");TextDec(t,attempts);TextPut(t,",\"complete\":false}");
}
static void Failure(TEXT *t,const CHAR8 *reason) {FailureCount(t,reason,0);}
static const CHAR8 *CaptureGuard(EFI_PCI_IO_PROTOCOL *pci,EFI_BOOT_SERVICES *bs,UINT32 words[16]) {
    EFI_STATUS status;UINT8 *config=(UINT8 *)words;UINTN segment=0,bus=0,device=0,function=0;UINT32 bar0;
    UINT64 supports=0,length=0;VOID *resource=NULL;BOOLEAN ok;
    if(!pci||!pci->Pci.Read||!pci->Mem.Read||!pci->GetLocation||!pci->GetBarAttributes||!bs||!bs->FreePool) return "missing_protocol";
    status=pci->Pci.Read(pci,EfiPciIoWidthUint32,0,16,words);
    if(EFI_ERROR(status)||!ExactTarget(config,64)||config[8]!=0xa1) return "identity_or_revision";
    /* The diagnostic never enables memory decoding or bus mastering itself. */
    if((Read16(config+4)&6)!=2) return "memory_decode_or_bus_master_state";
    status=pci->GetLocation(pci,&segment,&bus,&device,&function);
    if(EFI_ERROR(status)||segment!=0||bus!=1||device!=0||function!=0) return "unexpected_pci_location";
    /* BAR0 base is firmware-assigned: a literal address match rejects boards whose
       firmware relocates it (ours sits at 0xdd000000). Accept any nonzero 32-bit
       memory BAR and let the resource descriptor below be the authority. */
    bar0=Read32(config+16);
    if(!bar0||bar0==0xffffffffu||(bar0&1u)) return "bar0_relocated_or_invalid";
    status=pci->GetBarAttributes(pci,0,&supports,&resource);
    ok=(BOOLEAN)(!EFI_ERROR(status)&&ResourceMatches(resource,bar0,bar0,&length)&&length==0x01000000);
    if(resource) bs->FreePool(resource);
    if(!ok) return "bar0_descriptor_mismatch";
    return NULL;
}
UINTN SampleRegisters(TEXT *t,EFI_PCI_IO_PROTOCOL *pci,EFI_BOOT_SERVICES *bs) {
    UINT32 words[16],after[16],values[22],last_chip=0;
    EFI_STATUS statuses[22],status;UINTN i,attempts=0,completed=0;
    UINT64 length=0x01000000;BOOLEAN complete=TRUE,identity_stable=FALSE;
    const CHAR8 *reason=CaptureGuard(pci,bs,words);
    if(reason) {Failure(t,reason);return 0;}
    TextPut(t,"{\"guard_passed\":true,\"bar0_length\":");TextHex(t,length,16);
    TextPut(t,",\"samples\":[");
    for(i=0;i<sizeof(Registers)/sizeof(Registers[0]);i++) {
        if(Registers[i].offset>length-4||(Registers[i].offset&3)) {complete=FALSE;break;}
        values[i]=0;attempts++;
        statuses[i]=pci->Mem.Read(pci,EfiPciIoWidthUint32,0,Registers[i].offset,1,&values[i]);
        if(i) TextPut(t,",");
        TextPut(t,"{\"name\":\"");TextPut(t,Registers[i].name);TextPut(t,"\",\"offset\":");TextHex(t,Registers[i].offset,8);
        TextPut(t,",\"status\":");TextHex(t,statuses[i],16);TextPut(t,",\"value\":");
        if(EFI_ERROR(statuses[i])) TextPut(t,"null");else TextHex(t,values[i],8);TextPut(t,"}");
        completed++;
        if(EFI_ERROR(statuses[i])||(values[i]&0xffff0000u)==0xbadf0000u||
            (i==0&&(values[i]==0||values[i]==0xffffffffu))) {complete=FALSE;break;}
    }
    TextPut(t,"],\"final_pci_status\":");
    status=pci->Pci.Read(pci,EfiPciIoWidthUint32,0,16,after);TextHex(t,status,16);
    identity_stable=(BOOLEAN)(!EFI_ERROR(status)&&BytesEqual(words,after,sizeof(words)));
    TextPut(t,",\"pci_snapshot_unchanged\":");TextPut(t,identity_stable?"true":"false");
    TextPut(t,",\"final_chip\":");
    if(complete&&identity_stable) {
        attempts++;status=pci->Mem.Read(pci,EfiPciIoWidthUint32,0,0,1,&last_chip);
        if(EFI_ERROR(status)) TextPut(t,"null");else TextHex(t,last_chip,8);
        complete=(BOOLEAN)(!EFI_ERROR(status)&&last_chip==values[0]&&completed==22);
    } else {TextPut(t,"null");complete=FALSE;}
    TextPut(t,",\"read_attempts\":");TextDec(t,attempts);
    TextPut(t,",\"complete\":");TextPut(t,complete?"true":"false");
    TextPut(t,",\"unlock_eligibility\":false}");
    return attempts;
}

/* NV_PROM_DATA: dedicated ROM read window at BAR0 + 0x300000, max 1 MiB.
 * Reference: NVIDIA 610.43.03 kernel_gsp_vbios_tu102.c/dev_ext_devices.h.
 * This is a read, not a flash operation; no decode bit is changed.
 */
#define PROM_START 0x00300000u
#define PROM_BYTES 0x00100000u
static UINT32 PromWords[PROM_BYTES/4];
UINTN SampleProm(TEXT *t,EFI_PCI_IO_PROTOCOL *pci,EFI_BOOT_SERVICES *bs) {
    UINT32 words[16],after[16],chip=0,final_chip=0,signature=0;UINTN attempts=0,offset;
    EFI_STATUS status;BOOLEAN ok=TRUE,stable=FALSE;UINT8 hash[32];
    const CHAR8 *reason=CaptureGuard(pci,bs,words);
    if(reason) {Failure(t,reason);return 0;}
    attempts++;status=pci->Mem.Read(pci,EfiPciIoWidthUint32,0,0,1,&chip);
    if(EFI_ERROR(status)||chip!=0x194000a1u) {FailureCount(t,"unexpected_chip_for_prom",attempts);return attempts;}
    attempts++;status=pci->Mem.Read(pci,EfiPciIoWidthUint32,0,PROM_START,1,&signature);
    if(EFI_ERROR(status)||(signature&0xffff)!=0xaa55) {FailureCount(t,"prom_signature_unavailable",attempts);return attempts;}
    for(offset=0;offset<PROM_BYTES;offset+=4096) {
        attempts++;
        status=pci->Mem.Read(pci,EfiPciIoWidthUint32,0,PROM_START+offset,1024,PromWords+offset/4);
        if(EFI_ERROR(status)) {ok=FALSE;break;}
    }
    TextPut(t,"{\"guard_passed\":true,\"offset\":");TextHex(t,PROM_START,8);
    TextPut(t,",\"length\":");TextDec(t,PROM_BYTES);
    TextPut(t,",\"data_read_status\":");TextHex(t,status,16);
    status=pci->Pci.Read(pci,EfiPciIoWidthUint32,0,16,after);
    stable=(BOOLEAN)(!EFI_ERROR(status)&&BytesEqual(words,after,sizeof(words)));
    if(ok&&stable) {attempts++;status=pci->Mem.Read(pci,EfiPciIoWidthUint32,0,0,1,&final_chip);stable=(BOOLEAN)(!EFI_ERROR(status)&&final_chip==chip);}
    ok=(BOOLEAN)(ok&&stable&&Read32((UINT8 *)PromWords)==signature);
    TextPut(t,",\"identity_stable\":");TextPut(t,stable?"true":"false");
    TextPut(t,",\"complete\":");TextPut(t,ok?"true":"false");
    TextPut(t,",\"read_attempts\":");TextDec(t,attempts);
    TextPut(t,",\"sha256\":");if(ok) {Sha256((UINT8 *)PromWords,PROM_BYTES,hash);TextBytes(t,hash,32);}else TextPut(t,"null");
    TextPut(t,",\"hex\":");if(ok) TextBytes(t,(UINT8 *)PromWords,PROM_BYTES);else TextPut(t,"null");
    TextPut(t,",\"unlock_eligibility\":false}");
    return attempts;
}
