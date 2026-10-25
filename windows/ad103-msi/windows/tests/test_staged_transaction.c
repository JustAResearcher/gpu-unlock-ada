/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual unlock_610.c + actual Windows compat.c/port.c. Only EFI services and
 * hardware responses are mocked. The synthetic PROM is redistributable; this
 * test does NOT emulate signed GPU firmware or establish silicon support. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../../kernel/gpu_unlock/unlock_610.c"
#include "../../tests/vbios_fixture.h"
#include "../src/staged_state.h"

#define MAX_BUFFERS 24000
#define WARNING_STATUS ((EFI_STATUS)1)
enum scenario {
    SUCCESS, MAP_ERROR, MAP_SHORT, MAP_HIGH, MAP_UNALIGNED,
    MAP_SHORT_UNMAP_ERROR, MAP_SHORT_FREE_ERROR,
    ALLOCATE_WARNING, MAP_WARNING, UNMAP_WARNING, FREE_WARNING,
    ENABLE_ERROR, ENABLE_WARNING, DISABLE_ERROR, DISABLE_WARNING,
    MMIO_WARNING, CONFIG_WARNING, FLUSH_ERROR, FLUSH_WARNING,
    FWSEC_TIMEOUT, BOOTER_TIMEOUT, SELECTOR_READBACK, SELECTOR_LOST,
    READOUT_LOST, FLR_ERROR, FLR_WARNING, RESTORE_ERROR,
    PENDING_TIMEOUT, GSP_NOT_HALTED, WPR_STILL_ENABLED,
    SCENARIOS
};
static const char *names[SCENARIOS] = {
    "success", "map-error", "map-short", "map-high", "map-unaligned",
    "map-short-unmap-error", "map-short-free-error",
    "allocate-warning", "map-warning", "unmap-warning", "free-warning",
    "enable-error", "enable-warning", "disable-error", "disable-warning",
    "mmio-warning", "config-warning", "flush-error", "flush-warning",
    "fwsec-timeout", "booter-timeout", "selector-readback", "selector-lost",
    "readout-lost", "flr-error", "flr-warning", "restore-error",
    "pending-timeout", "gsp-not-halted", "wpr-still-enabled"
};
typedef struct pool_entry POOL_ENTRY;
struct pool_entry {POOL_ENTRY *next, *previous; max_align_t alignment;};
typedef struct {
    VOID *base, *cpu;
    UINTN pages;
    UINT64 dma;
    BOOLEAN mapped;
} BUFFER;
static struct {
    enum scenario scenario;
    UINT32 regs[0x1000000 / 4];
    UINT8 config[256];
    UINT64 attributes, next_dma, quiet_until;
    BUFFER buffers[MAX_BUFFERS];
    POOL_ENTRY *pools;
    struct unlock_transaction_work *work;
    unsigned allocated, maps, unmaps, frees, live, flushes;
    unsigned starts, gsp_starts, flrs, enables, disables, selectors;
    unsigned publication_checks, signature_checks, image_checks;
    unsigned restores, post_flr_gsp_writes;
    unsigned frees_at_fault;
    char log[65536];
} fake;
static EFI_BOOT_SERVICES bs;
static EFI_PCI_IO_PROTOCOL pci;
static struct pci_dev dev;
static u64 mask;

static UINT16 command(void) {UINT16 value;memcpy(&value,fake.config+4,2);return value;}
static void put16(UINTN offset, UINT16 value) {memcpy(fake.config+offset,&value,2);}
static void put32(UINTN offset, UINT32 value) {memcpy(fake.config+offset,&value,4);}
static void accessible(void) {assert(Gu.ticks>=fake.quiet_until);}
static bool scenario_is(enum scenario value) {return fake.scenario==value;}
static u64 get64(const void *p) {const u8 *s=p;u64 v=0;for(int i=7;i>=0;i--)v=(v<<8)|s[i];return v;}

VOID GuLog(const char *format, ...) {
    va_list args;size_t at=strlen(fake.log);va_start(args,format);
    vsnprintf(fake.log+at,sizeof(fake.log)-at,format,args);va_end(args);
}
VOID GuLogText(const char *text) {GuLog("%s",text);}
BOOLEAN GuLogReady(VOID) {return TRUE;}
static EFI_STATUS EFIAPI pool_allocate(EFI_MEMORY_TYPE type,UINTN bytes,VOID **out) {
    accessible();assert(type==EfiLoaderData && bytes);
    POOL_ENTRY *entry=calloc(1,sizeof(*entry)+bytes);assert(entry);
    entry->next=fake.pools;if(fake.pools)fake.pools->previous=entry;fake.pools=entry;
    *out=entry+1;
    if(bytes==sizeof(struct unlock_transaction_work) && !fake.work)fake.work=*out;
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI pool_free(VOID *memory) {
    accessible();assert(memory);
    POOL_ENTRY *entry=(POOL_ENTRY *)memory-1;
    if(entry->previous)entry->previous->next=entry->next;else fake.pools=entry->next;
    if(entry->next)entry->next->previous=entry->previous;
    if(memory==fake.work)fake.work=NULL;
    free(entry);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI stall(UINTN us) {(void)us;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI allocate_buffer(EFI_PCI_IO_PROTOCOL *p,EFI_ALLOCATE_TYPE kind,
    EFI_MEMORY_TYPE type,UINTN pages,VOID **cpu,UINT64 attributes) {
    accessible();assert(p==&pci && kind==AllocateAnyPages && type==EfiBootServicesData);
    assert(pages && !attributes && fake.allocated<MAX_BUFFERS);
    BUFFER *buffer=&fake.buffers[fake.allocated++];buffer->pages=pages;
    buffer->base=aligned_alloc(4096,(pages+1)*4096);assert(buffer->base);
    buffer->cpu=(u8 *)buffer->base+4096;
    memcpy((u8 *)buffer->cpu-sizeof(buffer),&buffer,sizeof(buffer));
    *cpu=buffer->cpu;fake.live++;
    return scenario_is(ALLOCATE_WARNING)?WARNING_STATUS:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI map_buffer(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_OPERATION operation,
    VOID *cpu,UINTN *bytes,EFI_PHYSICAL_ADDRESS *address,VOID **mapping) {
    accessible();assert(p==&pci && operation==EfiPciIoOperationBusMasterCommonBuffer);
    BUFFER *buffer;memcpy(&buffer,(u8 *)cpu-sizeof(buffer),sizeof(buffer));
    assert(cpu==buffer->cpu && *bytes==buffer->pages*4096 && !buffer->mapped);
    fake.maps++;
    if(scenario_is(MAP_ERROR) && fake.maps==6)return EFI_DEVICE_ERROR;
    buffer->dma=fake.next_dma;fake.next_dma+=buffer->pages*4096;
    buffer->mapped=TRUE;*address=buffer->dma;*mapping=buffer;
    if((scenario_is(MAP_SHORT) || scenario_is(MAP_SHORT_UNMAP_ERROR) ||
        scenario_is(MAP_SHORT_FREE_ERROR)) && fake.maps==6)*bytes-=4096;
    if(scenario_is(MAP_HIGH) && fake.maps==6)*address=0x100000000ULL;
    if(scenario_is(MAP_UNALIGNED) && fake.maps==6)*address+=1;
    return scenario_is(MAP_WARNING)?WARNING_STATUS:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI unmap_buffer(EFI_PCI_IO_PROTOCOL *p,VOID *mapping) {
    accessible();assert(p==&pci && mapping);BUFFER *buffer=mapping;
    assert(buffer->mapped && buffer->base);fake.unmaps++;
    assert(!Gu.started || (Gu.quiesced && !Gu.fault));
    assert(!(command()&4) && !Gu.master_attributes_pending);
    if(scenario_is(MAP_SHORT_UNMAP_ERROR))return EFI_DEVICE_ERROR;
    if(scenario_is(UNMAP_WARNING))return WARNING_STATUS;
    buffer->mapped=FALSE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI free_buffer(EFI_PCI_IO_PROTOCOL *p,UINTN pages,VOID *cpu) {
    accessible();assert(p==&pci && cpu);BUFFER *buffer;
    memcpy(&buffer,(u8 *)cpu-sizeof(buffer),sizeof(buffer));
    assert(buffer->base && pages==buffer->pages && !buffer->mapped);
    assert(!Gu.started || (Gu.quiesced && !Gu.fault));
    assert(!(command()&4) && !Gu.master_attributes_pending);
    if(scenario_is(MAP_SHORT_FREE_ERROR))return EFI_DEVICE_ERROR;
    if(scenario_is(FREE_WARNING))return WARNING_STATUS;
    free(buffer->base);buffer->base=buffer->cpu=NULL;fake.frees++;fake.live--;
    return EFI_SUCCESS;
}
static const u8 *dma_cpu(u64 address) {
    unsigned lo=0,hi=fake.allocated;
    while(lo<hi) {unsigned mid=lo+(hi-lo)/2;
        if(fake.buffers[mid].dma<address)lo=mid+1;else hi=mid;}
    assert(lo<fake.allocated);BUFFER *buffer=&fake.buffers[lo];
    assert(buffer->dma==address && buffer->mapped && buffer->base);return buffer->cpu;
}
static void check_publication(void) {
    struct unlock_transaction_work *w=fake.work;assert(w && w->published && Gu.started);
    assert((command()&4) && Gu.master_attributes_pending && !Gu.quiesced);
    const u8 *meta=dma_cpu(w->meta_dma);
    assert(get64(meta)==GU_WPR_META_MAGIC && get64(meta+8)==1);
    assert(get64(meta+72)==w->v67_dma && get64(meta+80)==GU_V67_SIZE);
    assert(!memcmp(dma_cpu(w->v67_dma),w->v67_fw->data,GU_V67_SIZE));
    assert(w->booter_image_size==56832 && w->boot_desc.image_size==36864);
    assert(!memcmp((u8 *)w->booter_cpu+w->booter_hdr.patch_loc,
                   w->booter_fw->data+72+w->booter_image_size,384));
    assert(w->permission_saved && w->permission_before[3]==0xffffff8f);
    fake.publication_checks++;
    if(!scenario_is(SUCCESS))return;
    u64 bytes=get64(meta+24);assert(bytes==84226048);
    for(u64 page=0,offset=0;offset<bytes;page++,offset+=4096) {
        const u8 *l0=dma_cpu(get64(meta+16));
        const u8 *l1=dma_cpu(get64(l0+(page>>18)*8));
        const u8 *l2=dma_cpu(get64(l1+((page>>9)&511)*8));
        const u8 *leaf=dma_cpu(get64(l2+(page&511)*8));
        size_t n=bytes-offset<4096?bytes-offset:4096;
        assert(!memcmp(leaf,w->gsp_fw->data+w->image_span.offset+offset,n));
        for(size_t i=n;i<4096;i++)assert(!leaf[i]);
    }
    fake.image_checks++;
}
static void check_signature(void) {
    struct unlock_transaction_work *w=fake.work;assert(w && !fake.frees);
    assert(!(command()&4) && !Gu.master_attributes_pending);
    assert(w->signature_calls==2 && w->sec2_halt_verified);
    const u8 *meta=dma_cpu(w->meta_dma);
    assert(get64(meta+72)==w->stock_dma && get64(meta+80)==4096);
    assert(!memcmp(dma_cpu(w->stock_dma),w->gsp_fw->data+w->sig_span.offset,4096));
    fake.signature_checks++;
}
static EFI_STATUS EFIAPI memory_read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT8 bar,UINT64 offset,UINTN count,VOID *out) {
    accessible();assert(p==&pci && !bar && width==EfiPciIoWidthUint32);
    assert(!(offset&3) && offset+count*4<=sizeof(fake.regs));
    memcpy(out,(u8 *)fake.regs+offset,count*4);
    return scenario_is(MMIO_WARNING)?WARNING_STATUS:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI memory_write(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT8 bar,UINT64 offset,UINTN count,VOID *data) {
    accessible();assert(p==&pci && !bar && width==EfiPciIoWidthUint32 && count==1);
    assert(!(offset&3) && offset+4<=sizeof(fake.regs));
    UINT32 value=*(UINT32 *)data;fake.regs[offset/4]=value;
    if(fake.flrs && offset==0x1103c0)fake.post_flr_gsp_writes++;
    if(offset==GU_FEAT_SS0_OFFSET || offset==GU_FEAT_SS1_OFFSET) {
        fake.selectors++;
        if(offset==GU_FEAT_SS1_OFFSET)fake.regs[offset/4]=value&15;
        if(scenario_is(SELECTOR_READBACK))fake.regs[offset/4]=0;
        if(fake.regs[GU_FEAT_SS0_OFFSET/4]==GU_SS0_UNLOCK &&
           fake.regs[GU_FEAT_SS1_OFFSET/4]==GU_SS1_UNLOCK)
            fake.regs[GU_FEAT_READOUT_1_OFFSET/4]=0;
    }
    if(offset==SEC2_BASE+0x118 || offset==0x110118)fake.regs[offset/4]=2;
    if(offset==SEC2_FALCON2+0x668 || offset==0x111668)fake.regs[offset/4]=1;
    if(offset==0x110100 || offset==0x110130) {
        fake.gsp_starts++;assert(fake.gsp_starts==1 && Gu.started);
        fake.regs[0x110100/4]=scenario_is(FWSEC_TIMEOUT)?2:0x10;
        u64 frts=fake.work->layout.frts_offset;
        fake.regs[0x1fa824/4]=(u32)(frts>>12)<<4;
        fake.regs[0x1fa828/4]=((u32)(frts>>12)<<4)+0xe00;
    }
    if(offset==SEC2_BASE+0x100 || offset==SEC2_BASE+0x130) {
        check_publication();fake.starts++;assert(fake.starts==1);
        fake.regs[0x1fa7cc/4]=0xfffff0ff;
        fake.regs[0x9a0148/4]=0xffffff0f; /* Observational PLM need not change. */
        fake.regs[0x1fa7c4/4]=~0u;
        fake.regs[GU_FEAT_PLM_OFFSET/4]=scenario_is(BOOTER_TIMEOUT)?0xffffff8f:~0u;
        fake.regs[(SEC2_BASE+0x40)/4]=scenario_is(BOOTER_TIMEOUT)?0:GU_AD102_MARKER;
        fake.regs[(SEC2_BASE+0x100)/4]=scenario_is(BOOTER_TIMEOUT)?2:0x10;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI config_read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT32 offset,UINTN count,VOID *data) {
    accessible();assert(p==&pci && width<=EfiPciIoWidthUint32 && count==1);
    assert(offset+(1u<<width)<=sizeof(fake.config));
    if(offset==(UINT32)Gu.pcie_cap+10 && scenario_is(PENDING_TIMEOUT) && fake.starts)
        put16(offset,PCI_EXP_DEVSTA_TRPND);
    memcpy(data,fake.config+offset,1u<<width);
    return scenario_is(CONFIG_WARNING)?WARNING_STATUS:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI config_write(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH width,
    UINT32 offset,UINTN count,VOID *data) {
    accessible();assert(p==&pci && width<=EfiPciIoWidthUint32 && count==1);
    assert(offset+(1u<<width)<=sizeof(fake.config));
    if(fake.flrs && offset>=0x10 && offset<=0x24) {
        assert(!(command()&3));
        assert(offset==0x24-4*fake.restores);fake.restores++;
        if(scenario_is(RESTORE_ERROR))return EFI_DEVICE_ERROR;
    }
    memcpy(fake.config+offset,data,1u<<width);
    if(offset==(UINT32)Gu.pcie_cap+8 && (*(UINT16 *)data&0x8000)) {
        check_signature();fake.flrs++;assert(fake.flrs==1 && Gu.flrs==1);
        fake.quiet_until=Gu.ticks+100000;
        put16(4,0);for(UINTN o=0x10;o<=0x24;o+=4)put32(o,0);
        fake.regs[(SEC2_BASE+0x3c0)/4]=fake.regs[0x1103c0/4]=0;
        fake.regs[GU_GFW_CPUCTL_OFFSET/4]=scenario_is(GSP_NOT_HALTED)?0:0x10;
        fake.regs[GU_GFW_PLM_OFFSET/4]=1;fake.regs[GU_GFW_BOOT_OFFSET/4]=0xff;
        if(!scenario_is(WPR_STILL_ENABLED))fake.regs[0x1fa824/4]=fake.regs[0x1fa828/4]=0;
        if(scenario_is(SELECTOR_LOST))fake.regs[GU_FEAT_SS0_OFFSET/4]=0;
        if(scenario_is(READOUT_LOST))fake.regs[GU_FEAT_READOUT_1_OFFSET/4]=1u<<24;
        if(scenario_is(FLR_ERROR))return EFI_DEVICE_ERROR;
        if(scenario_is(FLR_WARNING))return WARNING_STATUS;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI attributes(EFI_PCI_IO_PROTOCOL *p,
    EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION operation,UINT64 value,UINT64 *result) {
    accessible();assert(p==&pci);
    if(operation==EfiPciIoAttributeOperationSupported) {*result=EFI_PCI_IO_ATTRIBUTE_IO|
        EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER;return EFI_SUCCESS;}
    if(operation==EfiPciIoAttributeOperationGet){*result=fake.attributes;return EFI_SUCCESS;}
    assert(value==EFI_PCI_IO_ATTRIBUTE_BUS_MASTER && !result);
    if(operation==EfiPciIoAttributeOperationEnable) {
        fake.enables++;assert(fake.enables==1 && Gu.master_attributes_pending);
        assert(!(command()&4));fake.attributes|=value;put16(4,command()|4);
        if(scenario_is(ENABLE_ERROR))return EFI_DEVICE_ERROR;
        if(scenario_is(ENABLE_WARNING))return WARNING_STATUS;
    } else {
        assert(operation==EfiPciIoAttributeOperationDisable);fake.disables++;
        if(scenario_is(DISABLE_ERROR))return EFI_DEVICE_ERROR;
        if(scenario_is(DISABLE_WARNING))return WARNING_STATUS;
        fake.attributes&=~value;put16(4,command()&~4u);
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI flush(EFI_PCI_IO_PROTOCOL *p) {
    accessible();assert(p==&pci);fake.flushes++;
    if(scenario_is(FLUSH_ERROR))return EFI_DEVICE_ERROR;
    if(scenario_is(FLUSH_WARNING))return WARNING_STATUS;
    return EFI_SUCCESS;
}
static BOOLEAN display_check(void) {
    assert(fake.starts==1 && !fake.flrs && !(command()&4));return TRUE;
}
static void reset(enum scenario scenario) {
    memset(&fake,0,sizeof(fake));memset(&bs,0,sizeof(bs));memset(&pci,0,sizeof(pci));
    fake.scenario=scenario;fake.next_dma=0x10000000;
    fake.attributes=EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY;
    bs.AllocatePool=pool_allocate;bs.FreePool=pool_free;bs.Stall=stall;
    pci.AllocateBuffer=allocate_buffer;pci.FreeBuffer=free_buffer;
    pci.Map=map_buffer;pci.Unmap=unmap_buffer;pci.Flush=flush;pci.Attributes=attributes;
    pci.Mem.Read=memory_read;pci.Mem.Write=memory_write;
    pci.Pci.Read=config_read;pci.Pci.Write=config_write;
    GuInit(&bs,&pci);Gu.bar_base=GU_PROFILE_BAR_BASE;Gu.bar_size=sizeof(fake.regs);
    Gu.owned=Gu.armed=Gu.display_ok=TRUE;Gu.display_check=display_check;
    Gu.pm_cap=0x40;Gu.pcie_cap=0x60;Gu.pcie_version=2;
    put32(0,GU_BOARD_ID);put16(4,3);put32(8,0x030000a1);
    put32(0x10,GU_PROFILE_BAR_BASE);put32(0x18,0x8000000c);put32(0x1c,1);
    put32(0x20,0x9000000c);put32(0x24,1);put32(0x2c,GU_BOARD_SUBSYSTEM);
    put32(0x64,1u<<28);put16(0x68,0x2810);put16(0x88,0x50);
    mask=0xffffffffffffULL;
    dev=(struct pci_dev){.dev={.dma_mask=&mask,.coherent_dma_mask=0xfffffffffULL},
        .vendor=0x10de,.device=GU_AD103_RTX_4070_TI_SUPER,.current_state=PCI_D0,
        .pm_cap=Gu.pm_cap,.refs=1};
    fake.regs[0]=GU_BOARD_CHIP;
    fake.regs[(SEC2_BASE+0xf4)/4]=fake.regs[0x1100f4/4]=1u<<31;
    fake.regs[(SEC2_BASE+0x118)/4]=fake.regs[0x110118/4]=2;
    fake.regs[0x1fa7cc/4]=fake.regs[0x1fa7c4/4]=0x0004cb8f;
    fake.regs[0x9a0148/4]=0xffffff0f;fake.regs[GU_FEAT_PLM_OFFSET/4]=0xffffff8f;
    fake.regs[GU_FEAT_SS0_OFFSET/4]=0x64023025;fake.regs[GU_FEAT_SS1_OFFSET/4]=7;
    fake.regs[GU_FEAT_READOUT_1_OFFSET/4]=0x410080;
    fake.regs[GU_GFW_PLM_OFFSET/4]=1;fake.regs[GU_GFW_BOOT_OFFSET/4]=0xff;
    fake.regs[GU_GFW_CPUCTL_OFFSET/4]=0x10;
    fake.regs[0x1183a4/4]=0x3ff8;fake.regs[0x625f04/4]=1;
    fake.regs[0x1fa7c8/4]=0x45cb8f;fake.regs[0x1fa82c/4]=0x3ff0000;
    fake.regs[0x1fa830/4]=0x3ff7ff0;
    fake.regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
    u8 *rom=(u8 *)fake.regs+0x300000;vbios_fixture(rom,6);
    rom[292]=0;rom[293]=4;rom[294]=9;
    u8 *dmem=rom+684+256;dmem[0]=1;dmem[1]=4;dmem[2]=8;dmem[3]=2;
    vbios_w32(dmem+4,4);vbios_w32(dmem+8,32);dmem[36]=3;dmem[38]=64;
    vbios_w32(dmem+40,128);vbios_w32(dmem+44,44);
}
static void dispose_host_fixture(void) {
    /* Tests have no DMA engines. Deliberately retained buffers may only be
     * disposed here after assertions; never via the production cleanup API. */
    for(unsigned i=0;i<fake.allocated;i++)free(fake.buffers[i].base);
    while(fake.pools){POOL_ENTRY *next=fake.pools->next;free(fake.pools);fake.pools=next;}
}
static void load_assets(const char *root) {
    const char *names[5]={BOOTER_FILE_AD102,V67_FILE_AD102,GSP_FILE,BOOT_FILE_AD102,BOOT_DESC_FILE_AD102};
    for(unsigned i=0;i<5;i++) {
        char path[4096];int n=snprintf(path,sizeof(path),"%s/%s",root,strrchr(names[i],'/')+1);
        assert(n>0 && (size_t)n<sizeof(path));FILE *file=fopen(path,"rb");assert(file);
        assert(!fseek(file,0,SEEK_END));long size=ftell(file);assert(size>0);
        assert(!fseek(file,0,SEEK_SET));u8 *data=malloc(size);assert(data);
        assert(fread(data,1,size,file)==(size_t)size && !fclose(file));
        GuAssets[i]=(GU_ASSET){names[i],{(size_t)size,data}};
    }
}
static void verify(enum scenario scenario,int result) {
    if(scenario==SUCCESS) {
        if(result || Gu.fault || Gu.retained)fprintf(stderr,"%s",fake.log);
        assert(!result && !Gu.fault && !Gu.retained && GuSafeToReturn());
        assert(Gu.started && Gu.quiesced && Gu.flrs==1 && Gu.mmio_writes);
        assert(fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1 && fake.restores==6);
        assert(fake.publication_checks==1 && fake.signature_checks==1 && fake.image_checks==1);
        assert(!fake.live && !fake.pools && !Gu.allocations && !Gu.dma_count);
        assert(fake.frees==fake.allocated && fake.unmaps==fake.maps && dev.refs==1);
        assert(command()==3 && !Gu.master_attributes_pending);
        assert(fake.regs[GU_FEAT_SS0_OFFSET/4]==GU_SS0_UNLOCK);
        assert(fake.regs[GU_FEAT_SS1_OFFSET/4]==GU_SS1_UNLOCK);
        assert(!(fake.regs[GU_FEAT_READOUT_1_OFFSET/4]&GU_SPEED_READOUT_MASK));
        assert(GuStagedPostState(TRUE)==EFI_SUCCESS);
        /* The read-only postcondition also works after the caller closes its
         * exclusive ownership. Full graph/driver callbacks have their own
         * integration fixture; these assignments do not emulate those calls. */
        Gu.owned=Gu.armed=FALSE;
        assert(GuStagedPostState(FALSE)==EFI_SUCCESS);
    } else {
        if(!result && !Gu.retained && !Gu.fault)
            fprintf(stderr,"UNEXPECTED SUCCESS scenario=%s\n%s",names[scenario],fake.log);
        /* Core can report zero when the void DMA cleanup API latches retention;
         * the actual handoff gate must require all four independent conditions. */
        assert(result || Gu.retained || Gu.fault || !GuSafeToReturn());
        assert(GuStagedPostState(FALSE)!=EFI_SUCCESS);
        assert(!(command()&4));
        if(scenario==SELECTOR_LOST || scenario==READOUT_LOST) {
            assert(result && Gu.started && Gu.quiesced && !Gu.retained);
            assert(!fake.live && !Gu.allocations && dev.refs==1);
        }
        if(scenario==MAP_SHORT || scenario==MAP_HIGH || scenario==MAP_UNALIGNED) {
            assert(result && !Gu.started && !Gu.retained && !fake.live && !Gu.allocations);
        }
        if(scenario==MAP_ERROR || scenario==ALLOCATE_WARNING || scenario==MAP_WARNING ||
           scenario==MAP_SHORT_UNMAP_ERROR || scenario==MAP_SHORT_FREE_ERROR ||
           scenario==UNMAP_WARNING || scenario==FREE_WARNING || scenario==DISABLE_ERROR ||
           scenario==DISABLE_WARNING || scenario==FWSEC_TIMEOUT || scenario==BOOTER_TIMEOUT ||
           scenario==SELECTOR_READBACK || scenario==FLR_ERROR || scenario==FLR_WARNING ||
           scenario==RESTORE_ERROR || scenario==PENDING_TIMEOUT || scenario==GSP_NOT_HALTED ||
           scenario==WPR_STILL_ENABLED) {
            assert(Gu.retained && !GuSafeToReturn() && fake.live && Gu.allocations);
            assert(!fake.frees); /* Published or uncertain buffers never recycled. */
        }
        if(scenario==FWSEC_TIMEOUT || scenario==BOOTER_TIMEOUT || scenario==PENDING_TIMEOUT)
            assert(Gu.ticks>=1000000);
        if(scenario==ENABLE_ERROR || scenario==ENABLE_WARNING)
            assert(!fake.starts && !fake.gsp_starts && fake.enables==1 && fake.disables);
    }
    assert(fake.flrs<=1 && !fake.post_flr_gsp_writes);
    printf("%-25s result=%d retained=%u started=%u quiesced=%u DMA=%u FLR=%u PASS\n",
        names[scenario],result,Gu.retained,Gu.started,Gu.quiesced,fake.allocated,fake.flrs);
}
int main(int argc,char **argv) {
    setbuf(stdout,NULL);assert(argc==2);load_assets(argv[1]);
    for(enum scenario scenario=SUCCESS;scenario<SCENARIOS;scenario++) {
        reset(scenario);struct gpu_unlock_610_shared shared={0};
        assert(!gpu_unlock_610_load_shared(&dev,&shared));
        int result=gpu_unlock_610_unlock(&dev,&shared);
        gpu_unlock_610_release_shared(&shared);
        /* The caller's final independent containment runs even after a failed
         * shared transaction. This is the actual adapter, not a mock shortcut. */
        EFI_STATUS contained=GuClearMaster();
        if(contained!=EFI_SUCCESS)assert(Gu.fault && Gu.retained);
        verify(scenario,result);dispose_host_fixture();
    }
    for(unsigned i=0;i<5;i++)free((void *)GuAssets[i].firmware.data);
    puts("real shared core + real Windows EFI adapters: PASS");return 0;
}
