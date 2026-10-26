// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#define __KERNEL__
#include "../kernel/gpu_unlock/unlock_610.c"
#include "vbios_fixture.h"
static u32 regs[0x1000000/4];
static const char *asset_root;
static struct unlock_transaction_work *last_work;
static bool full_active;
static void check_published_image(void);
static void check_restored_signature(void);
static struct {
    u16 cmd,saved;
    unsigned long now;
    int allocs,live,frees,fail_alloc,fail_mask,flrs,fail_reset,starts,boot_mode,bar_bad,barriers,writes;
    int gsp_starts,fwsec_mode,master_fail,master_stuck,selector_writes,ss1_writes,fw_live;
    int pending,lost_identity,corrupt_selector,alien_wpr;
    int fw_requests,fail_fw,fail_claim,fail_map,fail_save,fail_context,pins,selector_bad;
    int restore_mask_fail,late_devres;
    int group_requests,fail_group,group_live,group_before,cache_records,cache_released;
    int hashes,bad_hash;
    int switch_stuck,dma_commands,dma_stall_after;
    int reset_delay,reset_reads[2],reset_pending[2],reset_post_fault;
    int reset_locked_until_flr,hold_writes,bar_ready_after,bar_ready_reads,wpr_after;
    int post_gfw,post_gfw_reads,gsp_post_reset_writes;
    int fwsec_wpr;u32 fwsec_wpr_lo,fwsec_wpr_hi; /* WPR2 that FWSEC-FRTS enables */
    int apertures,regions;
    int ad_mode;
    char log[32768];
} fake;
void *kzalloc(size_t n,int flags) {
    if(fake.fail_context)return NULL;
    last_work=calloc(1,n);return last_work;
}
void test_device_log(const char *format, ...) {
    va_list args;
    size_t at=strlen(fake.log);
    va_start(args,format);
    vsnprintf(fake.log+at,sizeof(fake.log)-at,format,args);
    va_end(args);
}
static u64 mask;
static struct pci_dev dev;
static bool fake_ada_id(void) {
    return dev.device==GU_AD102_RTX_4090 ||
           dev.device==GU_AD103_RTX_4080_SUPER ||
           dev.device==GU_AD103_RTX_4080 ||
           dev.device==GU_AD103_RTX_4070_TI_SUPER ||
           dev.device==GU_AD103_RTX_4070 ||
           dev.device==GU_AD106_RTX_4060_TI_16GB ||
           dev.device==GU_AD107_RTX_4050_MOBILE ||
           dev.device==GU_AD104_RTX_4070 ||
           dev.device==GU_AD104_RTX_4070_TI ||
           dev.device==GU_AD102_RTX_4070_TI_SUPER ||
           dev.device==GU_AD107_RTX_4060_LAPTOP ||
           dev.device==GU_AD103M_RTX_4090_MOBILE ||
           dev.device==GU_AD106_RTX_4060_TI ||
           dev.device==GU_AD104_RTX_4070_SUPER ||
           dev.device==GU_AD107_RTX_4060 ||
           dev.device==GU_AD107_RTX_4060_MOBILE;
}
static u8 fw_image[4097];
static struct firmware fw={sizeof(fw_image),fw_image};
static const u8 *hash_data;
static size_t hash_size;
static u8 hash_result[SHA256_DIGEST_SIZE];
/* Model the kernel hash primitive, including injected digest mismatches. The
 * Python runner independently hashes real assets before enabling full runs. */
void sha256(const u8 *data,size_t size,u8 digest[SHA256_DIGEST_SIZE]) {
    assert(data==hash_data && size==hash_size);
    memcpy(digest,hash_result,SHA256_DIGEST_SIZE);
    if(++fake.hashes==fake.bad_hash)digest[0]^=1;
}
static void setup(struct unlock_transaction_work *w) {
    memset(&fake,0,sizeof(fake)); memset(regs,0,sizeof(regs)); memset(w,0,sizeof(*w));
    mask=0xffffffffffffull; dev=(struct pci_dev){.dev={.dma_mask=&mask,.coherent_dma_mask=0xfffffffffull},.vendor=0x10de,.device=0x2204,.refs=1};
    w->pdev=&dev; w->bar=regs; w->bar_len=sizeof(regs); w->mapped=true;
    hash_data=NULL;hash_size=0;memset(hash_result,0,sizeof(hash_result));
    fake.cmd=2;fake.saved=2;regs[(SEC2_BASE+0xf4)/4]=1u<<31;regs[(0x110000+0xf4)/4]=1u<<31;
    regs[(SEC2_BASE+0x118)/4]=2;regs[(0x110000+0x118)/4]=2;
    regs[GU_FEAT_SS0_OFFSET/4]=0x88888888;regs[GU_FEAT_SS1_OFFSET/4]=0;
    regs[0x1fa824/4]=0xfffffff0;regs[0x1fa828/4]=0;
    regs[0]=0x172000a1;
}
void *dma_alloc_coherent(struct device *d,size_t n,dma_addr_t *a,int flags) {
    fake.allocs++; if(fake.fail_alloc==fake.allocs)return NULL;
    void *p=calloc(1,n); assert(p);
    *a=(d->coherent_dma_mask<=0xffffffffull?0x100000ull:0x100000000ull)+fake.allocs*0x10000ull;
    assert(*a+n-1<=d->coherent_dma_mask);fake.live++;return p;
}
void dma_free_coherent(struct device *d,size_t n,void *p,dma_addr_t a) {
    assert(p && a>=0x100000ull);free(p);fake.live--;fake.frees++;
}
int dma_set_mask(struct device *d,u64 v) {
    *d->dma_mask=v;return fake.fail_mask==1 || (fake.restore_mask_fail && v>0xffffffffull)?-EIO:0;
}
int dma_set_coherent_mask(struct device *d,u64 v) { d->coherent_dma_mask=v;return fake.fail_mask==2?-EIO:0; }
void dma_wmb(void) { fake.barriers++; }
void usleep_range(unsigned long a,unsigned long b) {fake.now+=a;}
ktime_t ktime_get(void) {return fake.now;}
u32 readl(const void *p) {
    size_t n=((const u8 *)p-(const u8 *)regs)/4;assert(n<ARRAY_SIZE(regs));
    if(fake.flrs && !n && fake.bar_ready_after) {
        fake.bar_ready_reads++;
        if(fake.bar_ready_after<0 || fake.bar_ready_reads<fake.bar_ready_after)
            return fake.bar_ready_after==-2?0x12345678:0xbadf0000;
    }
    if(fake.flrs && n==GU_GFW_CPUCTL_OFFSET/4 && fake.post_gfw==1 && ++fake.post_gfw_reads==5) {
        regs[GU_GFW_CPUCTL_OFFSET/4]=0x10;regs[GU_GFW_BOOT_OFFSET/4]=0xff;
    }
    int eng=n==(SEC2_BASE+0x3c0)/4?0:n==0x1103c0/4?1:-1;
    if(eng>=0 && fake.reset_pending[eng] && ++fake.reset_reads[eng]>=fake.reset_delay) {
        regs[n]=1;fake.reset_pending[eng]=0;
    }
    return regs[n];
}
void writel(u32 v,void *p) {
    if(full_active && fake_ada_id()) {
        assert(last_work->permission_saved);
        assert(last_work->permission_before[0]==0x0004cb8f);
        assert(last_work->permission_before[1]==0xffffff0f);
        assert(last_work->permission_before[2]==0x0004cb8f);
        assert(last_work->permission_before[3]==0xffffff8f);
    }
    fake.writes++;
    size_t off=(u8 *)p-(u8 *)regs;assert(off<sizeof(regs)); regs[off/4]=v;
    if(fake.flrs && off==0x1103c0) {
        fake.gsp_post_reset_writes++;
        regs[GU_GFW_CPUCTL_OFFSET/4]=0; /* Holding GSP reset breaks RM's entry wait. */
    }
    if(off==GU_FEAT_SS0_OFFSET)fake.selector_writes++;
    if(off==GU_FEAT_SS0_OFFSET && fake.selector_bad==1)regs[off/4]=0;
    if(off==GU_FEAT_SS1_OFFSET){fake.ss1_writes++;regs[off/4]=v&0xf;} /* 4-bit register, as on the card */
    if(off==GU_FEAT_SS1_OFFSET && fake.selector_bad==2)regs[off/4]=0;
    if(fake_ada_id() &&
       (off==GU_FEAT_SS0_OFFSET || off==GU_FEAT_SS1_OFFSET) &&
       regs[GU_FEAT_SS0_OFFSET/4]==GU_SS0_UNLOCK && regs[GU_FEAT_SS1_OFFSET/4]==GU_SS1_UNLOCK &&
       fake.ad_mode!=7)regs[GU_FEAT_READOUT_1_OFFSET/4]=0;
    if(off==SEC2_BASE+0x118 || off==0x110118) {
        fake.dma_commands++;
        regs[off/4]=fake.dma_stall_after && fake.dma_commands>=fake.dma_stall_after?1:2;
    }
    if(off==SEC2_FALCON2+0x668 || off==0x111668)regs[off/4]=fake.switch_stuck?0:1;
    if(off==0x110100 || off==0x110130) {
        fake.gsp_starts++;
        if(fake.fwsec_wpr){regs[0x1fa824/4]=fake.fwsec_wpr_lo;regs[0x1fa828/4]=fake.fwsec_wpr_hi;}
        regs[0x110100/4]=fake.fwsec_mode==1?2:0x10;
    }
    if(off==SEC2_BASE+0x100 || off==SEC2_BASE+0x130) {
        if(full_active)check_published_image();
        fake.starts++;
        if(fake.boot_mode==0)regs[GU_FEAT_PLM_OFFSET/4]=~0u;
        if(fake.boot_mode==2)regs[(SEC2_BASE+0x100)/4]=~0u;
        if(fake.boot_mode==3)regs[(SEC2_BASE+0x100)/4]=0x10;
        if(fake_ada_id()) {
            regs[0x1fa7cc/4]=fake.ad_mode==4?~0u:0xfffff0ff;
            regs[0x9a0148/4]=fake.ad_mode==6?0xffffff0f:~0u;
            regs[0x1fa7c4/4]=fake.ad_mode==5?0x0004cb8f:~0u;
            regs[(SEC2_BASE+0x40)/4]=fake.ad_mode==1?0:fake.ad_mode==2?0x67b00002:GU_AD102_MARKER;
            regs[(SEC2_BASE+0x100)/4]=fake.ad_mode==10?0:0x10;
            if(fake.ad_mode==3)regs[GU_FEAT_PLM_OFFSET/4]=0xffffff8f;
            if(fake.ad_mode==11)regs[0x1fa7cc/4]=0xbadf5040;
            if(fake.ad_mode==12)regs[0]^=1;
            if(fake.ad_mode==13)regs[GU_FEAT_READOUT_1_OFFSET/4]=0xbadf5040;
            if(fake.ad_mode==14)regs[GU_FEAT_SS0_OFFSET/4]=~0u;
        }
    }
    if(fake.fail_reset==2 && (off==SEC2_BASE+0x3c0 || off==0x1103c0))regs[off/4]=0;
    if(fake.fail_reset==8 && (off==SEC2_BASE+0x3c0 || off==0x1103c0))regs[off/4]=0xbadf0001;
    if((off==SEC2_BASE+0x3c0 || off==0x1103c0) && (v&1) && !(fake.cmd&4)) {
        fake.hold_writes++;
        int eng=off==SEC2_BASE+0x3c0?0:1;
        if(fake.reset_delay) {
            regs[off/4]=0;fake.reset_reads[eng]=0;fake.reset_pending[eng]=1;
        }
        if(fake.flrs && fake.reset_post_fault) {
            regs[off/4]=fake.reset_post_fault==1?0:0xbadf0001;
            fake.reset_pending[eng]=0;
        }
        if(fake.reset_locked_until_flr && !fake.flrs) {
            regs[off/4]=0;fake.reset_pending[eng]=0;
        }
        if(fake.flrs && fake.post_gfw==4)regs[GU_GFW_CPUCTL_OFFSET/4]=0;
    }
}
void *devres_open_group(struct device *d,void *id,int flags) {
    if(++fake.group_requests==fake.fail_group)return NULL;
    assert(!fake.group_live && !id);
    fake.group_live=1;fake.group_before=d->devres_head.count;
    d->devres_head.count++; /* open group marker */
    return &fake.group_live;
}
int devres_release_group(struct device *d,void *id) {
    assert(fake.group_live && id==&fake.group_live);
    int released=d->devres_head.count-fake.group_before-1;
    fake.cache_released+=released;d->devres_head.count=fake.group_before;
    fake.group_live=0;return released;
}
int request_firmware(const struct firmware **f,const char *n,struct device *d) {
    if(++fake.fw_requests==fake.fail_fw)return -ENOENT;
    if(!asset_root)return -ENOENT;
    char path[4096];
    int length=snprintf(path,sizeof(path),"%s/%s",asset_root,strrchr(n,'/')+1);
    assert(length>0 && (size_t)length<sizeof(path));
    FILE *file=fopen(path,"rb");assert(file);
    assert(!fseek(file,0,SEEK_END));long size=ftell(file);assert(size>0);assert(!fseek(file,0,SEEK_SET));
    struct firmware *loaded=malloc(sizeof(*loaded));u8 *data=malloc(size);assert(loaded && data);
    assert(fread(data,1,size,file)==(size_t)size);fclose(file);
    *loaded=(struct firmware){size,data};*f=loaded;fake.fw_live++;
    hash_data=data;hash_size=size;
    bool pinned=false;
    for(unsigned i=0;i<ARRAY_SIZE(firmware_pins);i++)
        if(!strcmp(n,firmware_pins[i].name)) {
            memcpy(hash_result,firmware_pins[i].digest,sizeof(hash_result));pinned=true;break;
        }
    assert(pinned);
    /* Linux CONFIG_FW_CACHE: successful request adds a device resource;
     * release_firmware frees the image, not this suspend-cache record. */
    d->devres_head.count++;fake.cache_records++;return 0;
}
void release_firmware(const struct firmware *f) {if(f){free((void *)f->data);free((void *)f);fake.fw_live--;}}
void __module_get(void *p) {fake.pins++;}
int pci_read_config_word(struct pci_dev *p,int r,u16 *v) {*v=r==PCI_COMMAND?fake.cmd:r==PCI_VENDOR_ID?0x10de:0;return 0;}
int pci_read_config_dword(struct pci_dev *p,int r,u32 *v) {
    *v=r==PCI_VENDOR_ID?(fake.flrs && fake.lost_identity?~0u:(u32)p->device<<16|p->vendor):(u32)fake.bar_bad;return 0;
}
int pci_write_config_word(struct pci_dev *p,int r,u16 v) {assert(r==PCI_COMMAND);fake.cmd=v|(fake.master_stuck?4:0);return 0;}
int pcie_capability_read_word(struct pci_dev *p,int r,u16 *v) {*v=r==PCI_EXP_DEVSTA?fake.pending:0;return 0;}
int pcie_capability_read_dword(struct pci_dev *p,int r,u32 *v) {*v=PCI_EXP_DEVCAP_FLR;return 0;}
int pci_is_pcie(struct pci_dev *p) {return 1;}
int pci_select_bars(struct pci_dev *p,int r) {return 7;}
int pci_request_region(struct pci_dev *p,int bar,const char *n) {
    assert(!bar); /* only BAR0: the display card's framebuffer keeps BAR1 */
    if(fake.fail_claim)return -EBUSY;
    fake.regions++;return 0;
}
void pci_release_region(struct pci_dev *p,int bar) {assert(!bar && fake.regions==1);fake.regions--;}
int aperture_remove_conflicting_pci_devices(struct pci_dev *p,const char *n) {
    /* The framebuffer stays through FWSEC and Booter; it goes right before FLR. */
    assert(!fake.flrs && !(fake.cmd&4));
    if(full_active)assert(fake.starts==1);
    fake.apertures++;return 0;
}
int pci_enable_device_mem(struct pci_dev *p) {fake.cmd|=2;return 0;}
void pci_disable_device(struct pci_dev *p) {}
resource_size_t pci_resource_len(struct pci_dev *p,int r) {return sizeof(regs);}
void *pci_iomap(struct pci_dev *p,int r,unsigned long n) {return fake.fail_map?NULL:regs;}
void pci_iounmap(struct pci_dev *p,void *v) {}
void *pci_map_rom(struct pci_dev *p,size_t *n) {return NULL;}
void pci_unmap_rom(struct pci_dev *p,void *v) {}
int pci_save_state(struct pci_dev *p) {fake.saved=fake.cmd;return fake.fail_save?-EIO:0;}
void pci_restore_state(struct pci_dev *p) {assert(!(fake.saved&4));fake.cmd=fake.saved;}
void pci_set_master(struct pci_dev *p) {if(!fake.master_fail)fake.cmd|=4;}
void pci_clear_master(struct pci_dev *p) {fake.cmd&=~4;}
int pcie_flr(struct pci_dev *p) {
    if(full_active)check_restored_signature();
    if(full_active)assert(fake.apertures==1);
    fake.flrs++;
    if(fake.late_devres)p->dev.devres_head.count++;
    /* FLR releases engine reset; post-FLR assertion must work independently. */
    regs[(SEC2_BASE+0x3c0)/4]=regs[0x1103c0/4]=0;
    regs[GU_GFW_CPUCTL_OFFSET/4]=0x10;
    regs[GU_GFW_PLM_OFFSET/4]=1;regs[GU_GFW_BOOT_OFFSET/4]=0xff;
    if(fake.post_gfw==1 || fake.post_gfw==2) {
        regs[GU_GFW_CPUCTL_OFFSET/4]=0;regs[GU_GFW_BOOT_OFFSET/4]=0;
    }
    if(fake.post_gfw==3)regs[GU_GFW_PLM_OFFSET/4]=0xbadf0001;
    if(fake.wpr_after!=4)regs[0x1fa824/4]=regs[0x1fa828/4]=0;
    if(fake.corrupt_selector==1)regs[GU_FEAT_SS0_OFFSET/4]=0;
    if(fake.corrupt_selector==2)regs[GU_FEAT_SS1_OFFSET/4]=0;
    if(fake.ad_mode==8)regs[GU_FEAT_READOUT_1_OFFSET/4]=1u<<24;
    if(fake.ad_mode==16)regs[GU_FEAT_PLM_OFFSET/4]=0xffffff8f;
    if(fake.alien_wpr){regs[0x1fa824/4]=0x1000;regs[0x1fa828/4]=0x2000;}
    if(fake.wpr_after==1)regs[0x1fa824/4]=regs[0x1fa828/4]=0;
    if(fake.wpr_after==2){regs[0x1fa824/4]=0x2000;regs[0x1fa828/4]=0x1000;}
    if(fake.wpr_after==3)regs[0x1fa824/4]=0xbadf0000;
    return fake.fail_reset==1?-EIO:0;
}
struct pci_dev *pci_dev_get(struct pci_dev *p) {p->refs++;return p;}
void pci_dev_put(struct pci_dev *p) {p->refs--;}
static u64 get64(const void *p) {const u8 *s=p;u64 v=0;for(int i=7;i>=0;i--)v=(v<<8)|s[i];return v;}
static const u8 *dma_page(u64 address) {
    struct unlock_transaction_work *w=last_work;
    assert(address>=w->pages[0].dma && !((address-w->pages[0].dma)%0x10000));
    u64 index=(address-w->pages[0].dma)/0x10000;
    assert(index<w->pages_allocated && w->pages[index].dma==address);
    return w->pages[index].cpu;
}
static void check_published_image(void) {
    struct unlock_transaction_work *w=last_work;const u8 *meta=w->meta_cpu;
    assert(w->published && !fake.pins && (fake.cmd&4));
    assert(get64(meta)==GU_WPR_META_MAGIC && get64(meta+8)==1);
    assert(get64(meta+72)==w->v67_dma && get64(meta+80)==GU_V67_SIZE);
    assert(!memcmp(w->v67_cpu,w->v67_fw->data,GU_V67_SIZE));
    if(fake_ada_id()) {
        assert(!memcmp((u8 *)w->booter_cpu+w->booter_hdr.patch_loc,
                       w->booter_fw->data+72+w->booter_image_size,384));
        assert(w->booter_image_size==0xde00 && w->boot_desc.image_size==36864);
        if(dev.device==GU_AD103_RTX_4080) {
            assert(get64(meta+176)==(32ull<<30));
            assert(get64(meta+184)==0x7fff00000ull);
            assert(get64(meta+152)==0x7ffe00000ull);
            assert(get64(meta+168)==0x7fff00000ull);
        }
        return;
    }
    assert(get64(meta+112)==0x5f5400000ull && get64(meta+136)==0x5fada0000ull);
    assert(get64(meta+144)==0x5ffdfa000ull && get64(meta+152)==0x5ffe00000ull);
    assert(get64(meta+168)==0x5fff00000ull && get64(meta+176)==0x600000000ull);
    /* Independently walk the radix the way a firmware consumer does. Compare
     * every published image byte, including transitions across 512-entry tables. */
    u64 image_bytes=get64(meta+24);assert(image_bytes==84226048);
    for(u64 page=0,off=0;off<image_bytes;page++,off+=4096) {
        const u8 *level0=dma_page(get64(meta+16));
        const u8 *level1=dma_page(get64(level0+(page>>18)*8));
        const u8 *level2=dma_page(get64(level1+((page>>9)&511)*8));
        const u8 *leaf=dma_page(get64(level2+(page&511)*8));
        size_t bytes=image_bytes-off<4096?image_bytes-off:4096;
        assert(!memcmp(leaf,w->gsp_fw->data+w->image_span.offset+off,bytes));
        for(size_t i=bytes;i<4096;i++)assert(!leaf[i]);
    }
    assert(!memcmp(w->boot_cpu,w->boot_fw->data,w->boot_desc.image_size));
    /* SEC2 fuse v1 selects slot 0 of the two-signature Booter asset. */
    assert(!memcmp((u8 *)w->booter_cpu+35344,w->booter_fw->data+72+60416,384));
}
static void check_restored_signature(void) {
    struct unlock_transaction_work *w=last_work;const u8 *meta=w->meta_cpu;
    assert(w->signature_calls==2 && get64(meta+72)==w->stock_dma && get64(meta+80)==4096);
    assert(!memcmp(w->stock_cpu,w->gsp_fw->data+w->sig_span.offset,4096));
    assert(!(fake.cmd&4));
    /* All published buffers remain alive; FLR must precede engine holds. */
    assert(fake.live==fake.allocs && !fake.frees && !fake.hold_writes);
}
static void synthetic_rom(u8 *rom) {
    vbios_fixture(rom,6);
    rom[292]=0;rom[293]=4;rom[294]=9; /* GSP engine and FWSEC ucode */
    u8 *dmem=rom+684+256;
    dmem[0]=1;dmem[1]=4;dmem[2]=8;dmem[3]=2;
    vbios_w32(dmem+4,4);vbios_w32(dmem+8,32);
    dmem[36]=3;dmem[38]=64;
    vbios_w32(dmem+40,128);vbios_w32(dmem+44,44);
}
static void load_rom(const char *path) {
    u8 *rom=(u8 *)regs+0x300000;
    if(!path || !strcmp(path,"-")) { synthetic_rom(rom); return; }
    FILE *prom=fopen(path,"rb");
    assert(prom && fread(rom,1,1024*1024,prom)==1024*1024);
    assert(fgetc(prom)==EOF);fclose(prom);
}
int main(int argc, char **argv) {
    struct unlock_transaction_work w;
    /* Compare all pinned digest choices, reject wrong hashes, empty/oversized
     * input and unknown names without any firmware or device activity. */
    setup(&w);hash_data=fw.data;hash_size=fw.size;
    for(unsigned i=0;i<ARRAY_SIZE(firmware_pins);i++) {
        memcpy(hash_result,firmware_pins[i].digest,sizeof(hash_result));
        assert(!verify_asset(&dev.dev,&fw,firmware_pins[i].name));
        hash_result[0]^=1;
        assert(verify_asset(&dev.dev,&fw,firmware_pins[i].name)==-EBADMSG);
    }
    struct firmware invalid={0,fw_image};
    assert(verify_asset(&dev.dev,&invalid,GSP_FILE)==-EBADMSG);
    invalid.size=(128u<<20)+1;
    assert(verify_asset(&dev.dev,&invalid,GSP_FILE)==-EBADMSG);
    assert(verify_asset(&dev.dev,&fw,"unknown")==-EBADMSG);
    assert(!fake.writes && !fake.allocs);
    setup(&w);
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_GA102));
    assert(!strcmp(unlock_assets(&dev)->signature_section, ".fwsignature_ga10x"));
    assert(unlock_assets(&dev)->boot_image_size==24576);
    dev.device=GU_AD102_RTX_4090;
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->payload, V67_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->signature_section, ".fwsignature_ad10x"));
    assert(unlock_assets(&dev)->boot_image_size==36864);
    dev.device=GU_AD103_RTX_4070_TI_SUPER;
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->signature_section, ".fwsignature_ad10x"));
    assert(unlock_assets(&dev)->boot_image_size==36864);
    assert(is_ad102(&w));
    dev.device=GU_AD103_RTX_4070;
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->signature_section, ".fwsignature_ad10x"));
    assert(unlock_assets(&dev)->boot_image_size==36864);
    assert(is_ad102(&w));
    dev.device=GU_AD103_RTX_4080_SUPER;
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->signature_section, ".fwsignature_ad10x"));
    assert(unlock_assets(&dev)->boot_image_size==36864);
    assert(is_ad102(&w));
    dev.device=GU_AD104_RTX_4070;
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->payload, V67_FILE_AD102));
    assert(unlock_assets(&dev)->arch==GU_BOOTER_AD10X);
    assert(is_ad102(&w));
    const unsigned short ad106_ids[]={GU_AD106_RTX_4060_TI,GU_AD106_RTX_4060_TI_16GB};
    for(unsigned i=0;i<ARRAY_SIZE(ad106_ids);i++) {
        dev.device=ad106_ids[i];
        assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
        assert(!strcmp(unlock_assets(&dev)->boot, BOOT_FILE_AD102));
        assert(!strcmp(unlock_assets(&dev)->desc, BOOT_DESC_FILE_AD102));
        assert(!strcmp(unlock_assets(&dev)->payload, V67_FILE_AD102));
        assert(!strcmp(unlock_assets(&dev)->signature_section, ".fwsignature_ad10x"));
        assert(unlock_assets(&dev)->arch==GU_BOOTER_AD10X);
        assert(unlock_assets(&dev)->boot_image_size==36864);
        assert(is_ad102(&w));
        assert(unlock_transaction_selector(&w)==-EPROTO && !fake.writes);
    }
    dev.device=GU_AD104_RTX_4070_TI;
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->signature_section, ".fwsignature_ad10x"));
    assert(unlock_assets(&dev)->boot_image_size==36864);
    assert(is_ad102(&w));
    dev.device=GU_AD102_RTX_4070_TI_SUPER;
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->signature_section, ".fwsignature_ad10x"));
    assert(unlock_assets(&dev)->boot_image_size==36864);
    assert(is_ad102(&w));
    dev.device=GU_AD107_RTX_4060_LAPTOP;
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->signature_section, ".fwsignature_ad10x"));
    assert(unlock_assets(&dev)->boot_image_size==36864);
    assert(is_ad102(&w));
    dev.device=GU_AD103M_RTX_4090_MOBILE;
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->payload, V67_FILE_AD102));
    assert(unlock_assets(&dev)->arch==GU_BOOTER_AD10X);
    assert(is_ad102(&w));
    dev.device=GU_AD104_RTX_4070_SUPER;
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->signature_section, ".fwsignature_ad10x"));
    assert(unlock_assets(&dev)->boot_image_size==36864);
    assert(is_ad102(&w));
    dev.device=GU_AD107_RTX_4060;
    assert(!strcmp(unlock_assets(&dev)->booter, BOOTER_FILE_AD102));
    assert(!strcmp(unlock_assets(&dev)->signature_section, ".fwsignature_ad10x"));
    assert(unlock_assets(&dev)->boot_image_size==36864);
    assert(is_ad102(&w));
    dev.device=0x2204;
    regs[0x110118/4]=0x1230;
    assert(poll_us(&w,0x110118,2,2,100,true)==-ETIMEDOUT);
    assert(strstr(fake.log,"poll timeout: reg=0x110118 last=0x1230 mask=0x2 expected=0x2 budget_us=100"));
    assert(strstr(fake.log,"poll_us error=-110"));
    assert(!fake.writes);
    setup(&w);
    regs[0x110118/4]=~0u;
    assert(poll_us(&w,0x110118,2,2,100,true)==-EIO);
    assert(strstr(fake.log,"poll inaccessible: reg=0x110118 last=0xffffffff"));
    /* Exercise the dual-core reset path, including RISC-V -> Falcon switching
     * and the documented tolerated pre-reset timeout. */
    for(int mode=0;mode<8;mode++) {
        setup(&w);
        const struct falcon_eng eng={SEC2_BASE,SEC2_FBIF,SEC2_FALCON2};
        regs[(SEC2_BASE+0xf4)/4]|=1u<<10;
        regs[(SEC2_FALCON2+0x668)/4]=mode==0?1:0x10;
        if(mode==2)regs[(SEC2_FALCON2+0x668)/4]=0xbadf0001;
        if(mode==3)fake.switch_stuck=1;
        if(mode==4)regs[(SEC2_BASE+0xf4)/4]|=1u<<12;
        if(mode==5)regs[(SEC2_BASE+0xf4)/4]&=~(1u<<31);
        if(mode==6)regs[0]=~0u;
        if(mode==7)regs[(SEC2_BASE+0x3c0)/4]=0xbadf0001;
        int ret=falcon_reset(&w,&eng);
        assert((!ret)==(mode==0 || mode==1 || mode==5));
        if(mode==1)assert(regs[(SEC2_FALCON2+0x668)/4]==1);
        if(mode==5){
            assert(strstr(fake.log,"pre-reset timeout tolerated"));
            assert(strstr(fake.log,"poll timeout tolerated:"));
            assert(!strstr(fake.log,"poll timeout:"));
            assert(!strstr(fake.log,"poll_us error=-110"));
            assert(!strstr(fake.log,"failure unlock_610.c"));
        }
        assert(!fake.gsp_starts && !fake.starts && !fake.dma_commands);
    }
    for(int mode=0;mode<3;mode++) {
        setup(&w);
        if(mode==0)regs[(SEC2_BASE+0x118)/4]=1;
        else fake.dma_stall_after=1;
        assert(falcon_dma(&w,SEC2_BASE,0,0,0x100000,mode==2?256:512,0x614)==-ETIMEDOUT);
        assert(fake.dma_commands==(mode?1:0) && fake.now<=1000100);
        if(mode==1)assert(strstr(fake.log,"Falcon DMA progress: done=256 total=512"));
    }
    for(int mode=0;mode<4;mode++) {
        setup(&w);
        const struct falcon_eng eng={SEC2_BASE,SEC2_FBIF,SEC2_FALCON2};
        w.plm_entry=0x8f;
        if(mode==0)regs[(SEC2_FBIF+0x24)/4]=0xbadf0001;
        if(mode==1)regs[SEC2_FBIF/4]=~0u;
        if(mode==2)regs[(SEC2_BASE+0x100)/4]=~0u;
        if(mode==3)regs[(SEC2_BASE+0x100)/4]=1u<<6; /* CPUCTL alias path */
        int ret=falcon_hs(&w,&eng,0,256,0,0,256,256,0,0,1,3,0x100000,0,0,true);
        assert((!ret)==(mode==3));
        assert(fake.starts==(mode==3?1:0));
        if(mode<2)assert(!fake.dma_commands);
    }
    /* Reject DMA address truncation/wrap, and an empty length, before any
     * reset or transfer. Nothing rejected here may pin the buffers. */
    for(int bad=0;bad<5;bad++) {
        setup(&w);
        u32 dest=bad==0?0xffff00:0, off=bad==1?0xffffff00:0;
        dma_addr_t src=bad==2?(1ull<<49)-256:0x100000;
        u32 size=bad==3?0:512;
        if(bad==4)src++;
        assert(falcon_dma(&w,SEC2_BASE,dest,off,src,size,0x614)==-EINVAL);
        assert(!fake.writes && !w.firmware_started);
    }
    /* A length that is not a multiple of the block size is legal: the engine
     * moves ceil(size/256) blocks (s_dmaTransfer_GA102). AD102's FWSEC DMEM
     * is 0xd80, which needs this. */
    setup(&w);
    assert(!falcon_dma(&w,SEC2_BASE,0,0,0x100000,0xd80,0x614));
    assert(fake.dma_commands==14 && w.firmware_started);
    {
        const struct falcon_eng eng={SEC2_BASE,SEC2_FBIF,SEC2_FALCON2};
        setup(&w);
        assert(falcon_hs(&w,&eng,0,256,0x200000,0,256,256,0,0,1,3,
                         0x100000,0,0,false)==-ERANGE);
        assert(!fake.writes);
        /* Bad DMEM geometry must not leave an already-submitted IMEM DMA. */
        assert(falcon_hs(&w,&eng,0,256,0,0,256,256,0x1000000,0,1,3,
                         0x100000,0,0,false)==-EINVAL);
        assert(!fake.writes);
    }
    /* Actual PROM -> descriptor -> patch -> GSP DMA/start -> WPR validation.
     * Faults after firmware access retain DMA, without starting SEC2. */
    for(int mode=0;mode<10;mode++) {
        setup(&w);
        u8 *rom=(u8 *)regs+0x300000;
        load_rom(argc>=2?argv[1]:NULL);
        struct gpu_unlock_fwsec parsed;
        assert(!gpu_unlock_vbios_fwsec(rom,1024*1024,&parsed));
        w.layout.frts_offset=0x5ffe00000ull;w.layout.wpr_end=0x5fff00000ull;
        regs[0x1fa824/4]=(u32)(w.layout.frts_offset>>12)<<4;
        regs[0x1fa828/4]=(u32)((w.layout.wpr_end-4096)>>12)<<4;
        fake.fwsec_mode=mode;
        if(mode==2)regs[(0x1400+0x0e*4)/4]=1u<<16;
        if(mode==3)regs[0x1fa828/4]=0; /* disabled, low alone matched */
        if(mode==4)regs[0x1fa828/4]+=16; /* outside reservation */
        if(mode==5)regs[0x1fa824/4]-=16;
        if(mode==6)regs[(0x8241c0+8*4)/4]=0xffff; /* unavailable fuse version */
        if(mode==7)fake.fail_alloc=1;
        if(mode==8)fake.master_fail=1;
        if(mode==9)rom[parsed.sig_off-44+37]=1; /* wrong engine mask */
        int ret=unlock_transaction_fwsec(&w);
        assert(!fake.starts && !fake.flrs);
        if(mode==0) {
            assert(!ret && fake.gsp_starts==1);
            assert(strstr(fake.log,"FWSEC WPR2:"));
        } else {
            assert(ret);
            if(mode>=6)assert(!fake.gsp_starts && !w.firmware_started);
            else assert(fake.gsp_starts==1 && w.firmware_started);
        }
        if(w.firmware_started) {
            assert(unlock_transaction_unwind(&w)==-EBUSY && w.retained && fake.live==1);
            free_coherent(&w); /* harness disposal only */
        } else assert(!unlock_transaction_unwind(&w) && !fake.live);
    }
    /* Exercise the register-to-layout adapter with the pinned image geometry.
     * A range error is pre-DMA/pre-firmware; never "fix" it by skipping bounds. */
    for(int scenario=0;scenario<8;scenario++) {
        setup(&w);
        regs[0x1183a4/4]=24*1024;
        regs[0x1fa7c8/4]=1;
        regs[0x1fa82c/4]=0xfffffff0; /* disabled MMU lock: hi < lo */
        regs[0x1fa830/4]=0;
        w.image_span.size=0x5053000;
        w.boot_desc.image_size=24576;
        if(scenario==1)regs[0x625f04/4]=(u32)(((24ull<<30)-(1ull<<20))>>16)<<8 | 8;
        if(scenario==2)regs[0x625f04/4]=8; /* valid old low workspace -> VBIOS fallback */
        if(scenario==3 || scenario==4) {
            u64 low=(24ull<<30)-((scenario==3?2ull:200ull)<<20);
            regs[0x1fa82c/4]=(u32)(low>>12)<<4;
            regs[0x1fa830/4]=(u32)(((24ull<<30)-4096)>>12)<<4;
        }
        if(scenario==5)regs[0x1183a4/4]=128; /* insufficient usable FB */
        if(scenario==6)regs[0x625f04/4]=(u32)((24ull<<30)>>16)<<8 | 8; /* beyond FB */
        /* Captured cold GA102 lock: lo=256 MiB, hi exclusive is not FB top. */
        if(scenario==7) {
            regs[0x1183a4/4]=0x6000; regs[0x625f04/4]=1; regs[0x1fa7c8/4]=0x45cb8f;
            regs[0x1fa82c/4]=0x100000; regs[0x1fa830/4]=0x5cffff0;
            w.image_span.size=84226048;
        }
        int ret=unlock_transaction_layout(&w);
        assert(ret==(scenario>=4 && scenario<=6?-ERANGE:0));
        assert(strstr(fake.log,"layout inputs: fb_mb="));
        if(scenario==4) assert(strstr(fake.log,"reservations below pre-scrubbed"));
        if(scenario==5) assert(strstr(fake.log,"usable FB below 256 MiB"));
        if(scenario==6) assert(strstr(fake.log,"VGA base at or above usable FB"));
        if(scenario==7) assert(strstr(fake.log,"mmu lock ignored: high address is not the FB top"));
        if(scenario<4 || scenario==7) {
            assert(w.layout.nonwpr_start>=w.layout_in.usable_fb-(256ull<<20));
            assert(w.layout.wpr_end<=w.layout_in.vga_base);
            assert(w.layout.heap_size>=88ull<<20);
        }
        assert(!fake.writes && !fake.allocs && !fake.starts && !fake.flrs);
    }
    /* Cold entry: completed GFW, closed PLM and WPR2 down. A used card is refused. */
    for(int mode=0;mode<3;mode++) {
        setup(&w);
        regs[GU_GFW_PLM_OFFSET/4]=1;regs[GU_GFW_BOOT_OFFSET/4]=0xff;regs[GU_GFW_CPUCTL_OFFSET/4]=0x10;
        regs[GU_FEAT_PLM_OFFSET/4]=mode==1?~0u:0x8f;
        if(mode==2)regs[0x1fa828/4]=0x5ffef0;
        int ret=unlock_transaction_gfw(&w);
        assert(ret==(mode==0?0:mode==1?-EALREADY:-EBUSY));
        assert(!fake.writes && !fake.gsp_starts);
        if(mode==2)assert(strstr(fake.log,"card already initialised since power on"));
    }
    setup(&w); dev.dev_flags=PCI_DEV_FLAGS_NO_FLR_RESET;
    assert(unlock_transaction_claim(&w)==-EOPNOTSUPP && !fake.flrs && !fake.allocs);
    /* A setter may mutate a mask before returning failure; restore both. */
    for(int fail=1;fail<=2;fail++) {
        setup(&w); fake.fail_mask=fail;
        assert(unlock_transaction_dma(&w)==-EIO);
        fake.fail_mask=0;
        assert(!unlock_transaction_unwind(&w));
        assert(mask==0xffffffffffffull && dev.dev.coherent_dma_mask==0xfffffffffull);
    }
    /* Stock restoration must preserve layout and must not republish V67. */
    {
        setup(&w);
        struct unlock_transaction_page page={.dma=0x200000};
        u8 metadata[GU_WPR_META_SIZE], stock[4096], before[GU_WPR_META_SIZE];
        w.meta_cpu=metadata;w.stock_cpu=stock;w.stock_dma=0x400000;w.stock_size=4096;
        w.v67_dma=0x300000;w.boot_dma=0x500000;w.pages=&page;w.pages_allocated=1;
        w.layout_in=(struct gpu_unlock_layout_in){.usable_fb=24ull<<30,
            .vga_base=(24ull<<30)-(1ull<<20),.frts_size=1ull<<20,
            .image_size=4097,.boot_size=24576,.nonwpr_size=1ull<<20,.heap_request=88ull<<20};
        assert(!gpu_unlock_layout(&w.layout_in,&w.layout));w.image_span.size=4097;
        w.boot_desc=(struct gpu_unlock_boot_desc){.code_offset=6144,.data_offset=2048,.image_size=24576};
        assert(!unlock_transaction_signature(&w) && w.published && w.signature_calls==1);
        assert(get64(metadata+72)==w.v67_dma && get64(metadata+80)==GU_V67_SIZE);
        memcpy(before,metadata,sizeof(before));
        assert(!unlock_transaction_signature(&w) && w.signature_calls==2);
        assert(get64(metadata+72)==w.stock_dma && get64(metadata+80)==4096);
        assert(!memcmp(before,metadata,72) && !memcmp(before+88,metadata+88,sizeof(before)-88));
        assert(unlock_transaction_signature(&w)==-EINVAL && fake.barriers==2);
    }
    /* Exercise every coherent-allocation failure in a real multi-page radix. */
    for(int fail=0;fail<=5;fail++) {
        setup(&w);w.gsp_fw=&fw;w.image_span.size=sizeof(fw_image);
        assert(!gpu_unlock_radix_plan(w.image_span.size,&w.radix));fake.fail_alloc=fail;
        int ret=unlock_transaction_radix(&w);
        if(fail) {
            assert(ret==-ENOMEM);
            assert(strstr(fake.log,"DMA allocation failed: asset=radix-page bytes=4096"));
        } else {
            assert(ret==0 && w.pages_allocated==5);
            assert(get64(w.pages[0].cpu)==w.pages[1].dma);
            assert(get64(w.pages[1].cpu)==w.pages[2].dma);
            assert(get64(w.pages[2].cpu)==w.pages[3].dma);
            assert(get64((u8 *)w.pages[2].cpu+8)==w.pages[4].dma);
            assert(get64((u8 *)w.pages[2].cpu+16)==0);
        }
        free_coherent(&w);assert(fake.live==0);
    }
    /* Early PLM polling must not wait for HALT or accept inaccessible engines. */
    for(int mode=0;mode<4;mode++) {
        setup(&w);fake.boot_mode=mode;w.master=w.firmware_started=w.published=true;
        w.booter_hdr=(struct gpu_unlock_booter_hdr){.app_code_off=256,.app_code_size=256,
            .os_data_off=512,.os_data_size=256,.patch_loc=512,.engine_id=1,.ucode_id=3};
        w.booter_dma=0x100000;w.meta_dma=0x200000;w.plm_entry=0x8f;
        int ret=unlock_transaction_boot(&w);
        assert(fake.starts==1);
        if(mode==0)assert(!ret);
        if(mode==1)assert(ret==-ETIMEDOUT && fake.now<=5000100);
        if(mode==1)assert(strstr(fake.log,"early PLM failed: pci=0x220410de"));
        if(mode==2)assert(ret==-ENODEV);
        if(mode==3)assert(ret==-EIO);
    }
    /* Failed FLR/reset/BAR checks retain allocations; success excludes fetches. */
    for(int failure=0;failure<10;failure++) {
        setup(&w);w.firmware_started=true;w.original_cmd=2;w.cmd_valid=true;
        w.v67_cpu=dma_alloc_coherent(&dev.dev,GU_V67_SIZE,&w.v67_dma,0);
        fake.fail_reset=(failure<3 || failure==8)?failure:0;fake.bar_bad=failure==3;
        fake.pending=failure==4?PCI_EXP_DEVSTA_TRPND:0;
        fake.lost_identity=failure==5;fake.corrupt_selector=failure==6?1:failure==9?2:0;fake.alien_wpr=failure==7;
        w.signature_calls=2;regs[GU_FEAT_SS1_OFFSET/4]=0x8;
        int ret=unlock_transaction_reset(&w);
        if(failure==6 || failure==9) {
            /* Lost selector state fails the unlock, but proven reset permits cleanup. */
            assert(ret==-EIO && w.quiesced);
            assert(!unlock_transaction_unwind(&w) && !fake.live && !w.retained);
        } else if(failure) {
            assert(ret && !w.quiesced);
            assert(unlock_transaction_unwind(&w)==-EBUSY && w.retained && fake.live==1);
            assert(strstr(fake.log,"failure unlock_610.c:"));
            if(failure==1)assert(strstr(fake.log,"PCIe FLR returned: result=-5"));
            if(failure==3)assert(strstr(fake.log,"post-FLR BAR0: actual=0x1 expected=0"));
            /* Host harness disposal only; the kernel intentionally retains. */
            free_coherent(&w);
        } else {
            assert(!ret && w.quiesced && fake.flrs==1);
            assert(!(regs[(SEC2_BASE+0x3c0)/4]&1) && !regs[0x1103c0/4]);
            assert(!unlock_transaction_unwind(&w) && fake.live==0);
        }
    }
    /* Entire production transaction with pinned assets and a board ROM (a real
     * dump, or "-" for the synthetic fixture). Only PCI/MMIO/DMA are mocked;
     * parsers, patching and ordering are real. */
    if(argc==3) {
        asset_root=argv[2];
        full_active=true;
        static const char *cases[]={"success","already-full","context-oom","asset-1","asset-2",
            "asset-3","asset-4","asset-5","claim-busy","map-failed","save-failed",
            "dma-1","dma-6","dma-7","dma-8","dma-48","dma-last",
            "gfw-timeout","preexisting-plm","fwsec-timeout","fwsec-error","fwsec-wpr",
            "booter-timeout","booter-inaccessible","booter-halted","selector-readback",
            "flr-failed","reset-inaccessible","pci-lost","pending","wpr-alien","selector-lost","mask-restore","decode-off",
            "reset-delay10","reset-delay12","reset-never","reset-post-never","reset-post-inaccessible",
            "reset-locked-before-flr","bar-ready-late","bar-inaccessible","bar-wrong-chip",
            "wpr-disabled-zero","wpr-inverted","wpr-inaccessible",
            "post-gfw-late","post-gfw-timeout","post-gfw-inaccessible","post-gsp-resumed",
            "wpr-frts-retained","firmware-group-oom-first","firmware-group-oom-later","devres-preexisting","devres-at-handoff",
            "warm-wpr2","ss1-readback","ss1-lost","power-unknown","power-unknown-no-pm",
            "firmware-bme","firmware-bme-stuck",
            "hash-gsp","hash-payload","hash-boot","hash-desc","hash-booter"};
        int total_allocations=0;
        for(unsigned scenario=0;scenario<ARRAY_SIZE(cases);scenario++) {
            setup(&w);
            load_rom(argv[1]);
            regs[0x1183a4/4]=0x6000;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
            regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cffff0;
            regs[GU_GFW_PLM_OFFSET/4]=1;regs[GU_GFW_BOOT_OFFSET/4]=0xff;
            regs[GU_GFW_CPUCTL_OFFSET/4]=0x10;
            /* Representative cold RTX 4090 register values. */
            regs[GU_FEAT_PLM_OFFSET/4]=0xffffff8f;regs[GU_FEAT_SS0_OFFSET/4]=0x40171022;
            regs[GU_FEAT_SS1_OFFSET/4]=0x2;
            fake.fwsec_wpr=1;
            fake.fwsec_wpr_lo=(u32)(0x5ffe00000ull>>12)<<4;
            fake.fwsec_wpr_hi=(u32)(0x5ffeff000ull>>12)<<4;
            regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
            /* FWSEC fuse: the real board ROM needs version 1, the fixture 0. */
            if(strcmp(argv[1],"-"))regs[(0x8241c0+8*4)/4]=1;
            if(scenario==1){regs[GU_FEAT_SS0_OFFSET/4]=0x88888888;regs[GU_FEAT_SS1_OFFSET/4]=0x8;}
            if(scenario==2)fake.fail_context=1;
            if(scenario>=3 && scenario<=7)fake.fail_fw=scenario-2;
            if(scenario==8)fake.fail_claim=1;
            if(scenario==9)fake.fail_map=1;
            if(scenario==10)fake.fail_save=1;
            if(scenario>=11 && scenario<=16) {
                const int failures[]={1,6,7,8,48,total_allocations};
                fake.fail_alloc=failures[scenario-11];
            }
            if(scenario==17)regs[GU_GFW_BOOT_OFFSET/4]=0;
            if(scenario==18)regs[GU_FEAT_PLM_OFFSET/4]=~0u;
            if(scenario==19)fake.fwsec_mode=1;
            if(scenario==20)regs[(0x1400+0x0e*4)/4]=1u<<16;
            if(scenario==21)fake.fwsec_wpr_hi=0;
            if(scenario>=22 && scenario<=24)fake.boot_mode=scenario-21;
            if(scenario==25)fake.selector_bad=1;
            if(scenario==26)fake.fail_reset=1;
            if(scenario==27)fake.fail_reset=8;
            if(scenario==28)fake.lost_identity=1;
            if(scenario==29)fake.pending=PCI_EXP_DEVSTA_TRPND;
            if(scenario==30)fake.alien_wpr=1;
            if(scenario==31)fake.corrupt_selector=1;
            if(scenario==32)fake.restore_mask_fail=1;
            if(scenario==33)fake.cmd=0;
            if(scenario==34)fake.reset_delay=10;
            if(scenario==35)fake.reset_delay=12;
            if(scenario==36)fake.fail_reset=2;
            if(scenario==37)fake.reset_post_fault=1;
            if(scenario==38)fake.reset_post_fault=2;
            if(scenario==39)fake.reset_locked_until_flr=1;
            if(scenario==40)fake.bar_ready_after=5;
            if(scenario==41)fake.bar_ready_after=-1;
            if(scenario==42)fake.bar_ready_after=-2;
            if(scenario>=43 && scenario<=45)fake.wpr_after=scenario-42;
            if(scenario>=46 && scenario<=49)fake.post_gfw=scenario-45;
            if(scenario==50)fake.wpr_after=4;
            if(scenario==51)fake.fail_group=1;
            if(scenario==52)fake.fail_group=3;
            if(scenario==53)dev.dev.devres_head.count=1;
            if(scenario==54)fake.late_devres=1;
            if(scenario==55)regs[0x1fa828/4]=fake.fwsec_wpr_hi; /* a driver already ran FWSEC */
            if(scenario==56)fake.selector_bad=2;
            if(scenario==57)fake.corrupt_selector=2;
            if(scenario==58 || scenario==59)dev.current_state=PCI_UNKNOWN;
            if(scenario==58)dev.pm_cap=0x60;
            if(scenario==60 || scenario==61)fake.cmd=6; /* firmware left BME set */
            if(scenario==61)fake.master_stuck=1;
            if(scenario>=62)fake.bad_hash=scenario-61;
            /* As main.c: the GSP file is loaded once, ahead of every card. */
            struct gpu_unlock_610_shared shared={0};last_work=NULL;
            int ret=gpu_unlock_610_load_shared(&dev,&shared);
            if(!ret)ret=gpu_unlock_610_unlock(&dev,&shared);
            gpu_unlock_610_release_shared(&shared);
            bool expected=scenario<2 || (scenario>=33 && scenario<=35) || scenario==39 || scenario==40 || scenario==43 || scenario==46 ||
                scenario==58 || scenario==60;
            bool retained=fake.pins;
            if((!ret)!=expected)fprintf(stderr,"%s\n%s: error=%d\n",fake.log,cases[scenario],ret);
            assert((!ret)==expected);
            if(expected) {
                assert(fake.gsp_starts==1 && fake.starts==1 && fake.flrs==1 && !fake.pins);
                assert(fake.apertures==1 && !fake.regions);
                assert(fake.selector_writes==(scenario==1?0:1) && fake.ss1_writes==(scenario==1?0:1));
                assert(regs[GU_FEAT_SS0_OFFSET/4]==0x88888888 && regs[GU_FEAT_SS1_OFFSET/4]==0x8);
                assert(mask==0xffffffffffffull && dev.dev.coherent_dma_mask==0xfffffffffull);
                /* Mirror Linux really_probe: driver entry rejects nonempty devres. */
                assert(list_empty(&dev.dev.devres_head));
                assert(fake.cache_records==5 && fake.cache_released==5);
                /* Pinned RM checks GSP halt + readable completed GFW BEFORE
                 * its own reset/bootstrap. Preserve that entry contract. */
                assert((regs[GU_GFW_CPUCTL_OFFSET/4]&0x10) && (regs[GU_GFW_PLM_OFFSET/4]&1));
                assert((regs[GU_GFW_BOOT_OFFSET/4]&0xff)==0xff && !regs[0x1103c0/4]);
                assert(!(regs[0x1fa828/4]>>4)); /* Stock RM rejects any active WPR2. */
                if(!scenario)total_allocations=fake.allocs;
            }
            if(scenario>=34 && scenario<=50) {
                assert(fake.starts==1 && fake.flrs==1);
                if(!expected)assert(retained && !last_work->quiesced);
                if(scenario==41 || scenario==42)assert(!fake.hold_writes && ret==-ETIMEDOUT);
                if(scenario==39)assert(fake.hold_writes==1);
                if(scenario==47 || scenario==48)assert(!fake.hold_writes && ret==-ETIMEDOUT);
            }
            assert(!fake.gsp_post_reset_writes);
            assert(fake.gsp_starts<=1 && fake.starts<=1 && fake.flrs<=1 && fake.pins<=1);
            assert(fake.selector_writes<=1 && fake.ss1_writes<=1);
            if(retained) {
                assert(last_work->retained && fake.live>0 && dev.refs==2);
                /* Harness-only disposal of intentionally retained production state. */
                last_work->firmware_started=false;fake.restore_mask_fail=0;
                assert(!unlock_transaction_unwind(last_work));
                pci_dev_put(last_work->pdev);free(last_work);
            }
            /* A card whose BME will not clear is left as found. */
            assert(!fake.live && !fake.fw_live && dev.refs==1 && (scenario==61 || !(fake.cmd&4)));
            assert(!fake.group_live && fake.cache_records==fake.cache_released);
            assert(dev.dev.devres_head.count==(scenario==53 || scenario==54?1u:0u));
            if(scenario>=51 && scenario<=53)assert(!fake.writes && !fake.allocs && !fake.flrs);
            if(scenario==53)assert(fake.fw_requests==1 && ret==-EBUSY); /* only the shared load */
            if(scenario==3 || scenario==51)assert(!last_work && fake.fw_requests==(scenario==3));
            if(scenario==54)assert(fake.flrs==1 && ret==-EBUSY);
            if(scenario==53 || scenario==54)assert(strstr(fake.log,"driver probe resources: phase=handoff empty=0"));
            if(scenario==33)assert(fake.cmd==0);
            if(scenario==56)assert(ret==-EIO && retained && fake.ss1_writes==1 && !fake.flrs);
            if(scenario==57)assert(ret==-EIO && !retained && fake.flrs==1);
            if(scenario==59)assert(ret==-EOPNOTSUPP && !fake.allocs && !fake.flrs);
            if(scenario==60)assert(strstr(fake.log,"claim: cleared firmware BME"));
            if(scenario==61)assert(ret==-EBUSY && !fake.allocs && !fake.flrs);
            if(scenario>=62)assert(ret==-EBADMSG && !retained && !fake.writes && !fake.allocs && !fake.regions);
            if(scenario==55) {
                assert(ret==-EBUSY && !retained && !fake.gsp_starts && !fake.starts && !fake.flrs);
                assert(strstr(fake.log,"card already initialised since power on"));
            }
            printf("full transaction %s: error=%d retained=%u PASS\n",cases[scenario],ret,retained);
        }
        puts("pinned firmware full BF16 transactions passed under mocked hardware");
    }
    puts("actual unlock_transaction radix, early-PLM and reset/retention checks passed");
    return 0;
}
