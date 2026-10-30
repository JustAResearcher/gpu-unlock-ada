/* SPDX-License-Identifier: GPL-2.0-only */
/* Reuse the PCI/MMIO/DMA harness; no hardware or driver is accessed. */
#define main ga102_fixture_main
#include "test_unlock_adapter.c"
#undef main

static u8 payload[GU_V67_SIZE];
static const struct firmware payload_fw = { sizeof(payload), payload };

static void ad102_setup(struct unlock_transaction_work *w)
{
    setup(w);
    dev.device=GU_AD102_RTX_4090;
    regs[0]=0x192000a1;
    regs[0x1fa7cc/4]=regs[0x1fa7c4/4]=0x0004cb8f;
    regs[0x9a0148/4]=0xffffff0f;
    regs[GU_FEAT_PLM_OFFSET/4]=0xffffff8f;
    regs[GU_FEAT_SS0_OFFSET/4]=0x64023025;
    regs[GU_FEAT_SS1_OFFSET/4]=7;
    regs[GU_FEAT_READOUT_1_OFFSET/4]=0x410080;
    regs[GU_GFW_PLM_OFFSET/4]=1;
    regs[GU_GFW_BOOT_OFFSET/4]=0xff;
    regs[GU_GFW_CPUCTL_OFFSET/4]=0x10;
}

static void prepare_boot(struct unlock_transaction_work *w)
{
    assert(!unlock_transaction_gfw(w) && w->permission_saved && !fake.writes);
    w->master=w->firmware_started=w->published=true;
    w->booter_hdr=(struct gpu_unlock_booter_hdr){.app_code_off=256,.app_code_size=256,
        .os_data_off=512,.os_data_size=256,.patch_loc=512,.engine_id=1,.ucode_id=3};
    w->booter_dma=0x100000;w->meta_dma=0x200000;
    w->v67_cpu=dma_alloc_coherent(&dev.dev,GU_V67_SIZE,&w->v67_dma,0);
    w->v67_fw=&payload_fw;
}

int main(int argc, char **argv)
{
    struct unlock_transaction_work w;
    /* Required permissions are exact; FBPA is observational. Missing/wrong
     * markers, ignored stores, live SEC2 and inaccessible BAR views all stop
     * before either selector. No failure grants DMA cleanup authority. */
    static const int modes[]={0,1,2,3,4,5,6,10,11,12,13,14};
    for(unsigned i=0;i<ARRAY_SIZE(modes);i++) {
        ad102_setup(&w);prepare_boot(&w);fake.ad_mode=modes[i];
        int ret=unlock_transaction_boot(&w);
        bool success=fake.ad_mode==0 || fake.ad_mode==6;
        if((!ret)!=success)fprintf(stderr,"%s\nmode=%d ret=%d\n",fake.log,fake.ad_mode,ret);
        assert((!ret)==success && w.payload_verified==success && fake.starts==1);
        assert(strstr(fake.log,"AD102 payload result:") && strstr(fake.log,"after-payload: WPR_CFG"));
        assert(!fake.selector_writes && !fake.ss1_writes && !fake.flrs);
        assert(w.permission_before[0]==0x0004cb8f && w.permission_before[2]==0x0004cb8f);
        if(!success)assert(unlock_transaction_selector(&w)==-EPROTO);
        if(fake.ad_mode==10)assert(ret==-ETIMEDOUT && !w.sec2_halt_verified);
        assert(unlock_transaction_unwind(&w)==-EBUSY && w.retained && fake.live==1 && !fake.frees);
        free_coherent(&w); /* harness disposal only */
    }
    /* Rejected cold permission/readout views never start firmware. */
    for(int mode=0;mode<5;mode++) {
        ad102_setup(&w);
        if(mode<4)regs[ad102_permissions[mode].offset/4]=0xbadf5040;
        else regs[GU_FEAT_READOUT_1_OFFSET/4]=~0u;
        assert(unlock_transaction_gfw(&w)==-ENODEV);
        assert(!fake.writes && !fake.starts && !fake.gsp_starts);
    }
    /* Verify the DMA-visible payload after reset and before the first SEC2
     * transfer/start, including a corrupted payload copy. */
    ad102_setup(&w);prepare_boot(&w);((u8 *)w.v67_cpu)[1]^=1;
    assert(unlock_transaction_boot(&w)==-EIO);
    assert(!fake.starts && !fake.dma_commands && !w.sec2_started);
    free_coherent(&w);
    /* A marker is emitted before cleanup: never mutate metadata while the
     * engine might still consume it. Verified HALT permits restore, not free. */
    {
        u8 metadata[GU_WPR_META_SIZE], before[GU_WPR_META_SIZE], stock[4096];
        ad102_setup(&w);memset(metadata,0x5a,sizeof(metadata));memcpy(before,metadata,sizeof(before));
        w.meta_cpu=metadata;w.stock_cpu=stock;w.pages_allocated=1;w.signature_calls=1;
        w.stock_dma=0x400000;w.stock_size=4096;w.sec2_started=true;
        assert(unlock_transaction_signature(&w)==-EBUSY);
        assert(!memcmp(metadata,before,sizeof(metadata)) && w.signature_calls==1 && !fake.barriers);
        assert(strstr(fake.log,"stock signature restore deferred"));
        w.sec2_halt_verified=true;
        assert(!unlock_transaction_signature(&w) && w.signature_calls==2);
        assert(get64(metadata+72)==0x400000 && get64(metadata+80)==4096);
    }
    /* Nine low 3-bit fields must be zero; reserved high bits are irrelevant.
     * A mismatch before FLR consumes no reset, while a lost state after a
     * proven FLR fails verification but still permits safe cleanup. */
    for(int mode=0;mode<7;mode++) {
        ad102_setup(&w);w.payload_verified=true;w.firmware_started=true;
        fake.ad_mode=mode==1?7:mode==2?8:mode==6?16:0;
        if(mode==3)fake.selector_bad=1;
        if(mode==4)fake.selector_bad=2;
        assert(!ad102_sample_permissions(&w,"before-firmware",w.permission_before,true));
        w.permission_saved=true;
        int ret=unlock_transaction_selector(&w);
        if(mode==1 || mode==3 || mode==4) {
            assert(ret==-EIO && !fake.flrs);
            continue;
        }
        assert(!ret);
        regs[GU_FEAT_PLM_OFFSET/4]=~0u;
        if(mode==5)regs[GU_FEAT_READOUT_1_OFFSET/4]=0xf8000000;
        w.signature_calls=2;
        ret=unlock_transaction_reset(&w);
        assert((!ret)==(mode==0 || mode==5));
        assert(w.quiesced && fake.flrs==1);
        if(mode==2 || mode==6)assert(ret==-EIO);
        assert(!unlock_transaction_unwind(&w) && !w.retained);
    }
    /* Real asset parsing, pinning, publication, restore, FLR and retention;
     * all register behavior is supplied by this host-only harness. */
    if(argc==3) {
        asset_root=argv[2];full_active=true;
        static const int cases[]={0,1,2,4,6,7,8,10};
        for(unsigned i=0;i<ARRAY_SIZE(cases);i++) {
            ad102_setup(&w);fake.ad_mode=cases[i];load_rom(argv[1]);
            regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
            regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
            regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
            fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
            struct gpu_unlock_610_shared shared={0};last_work=NULL;
            assert(!gpu_unlock_610_load_shared(&dev,&shared));
            int ret=gpu_unlock_610_unlock(&dev,&shared);
            gpu_unlock_610_release_shared(&shared);
            bool success=cases[i]==0 || cases[i]==6;
            if((!ret)!=success)fprintf(stderr,"%s\nfull AD mode=%d ret=%d\n",fake.log,cases[i],ret);
            assert((!ret)==success && fake.starts==1 && fake.gsp_starts==1);
            if(cases[i]==1 || cases[i]==2 || cases[i]==4 || cases[i]==10)
                assert(!fake.selector_writes && !fake.ss1_writes && !fake.flrs);
            if(cases[i]==10) {
                assert(fake.pins && last_work->signature_calls==1);
                assert(get64((u8 *)last_work->meta_cpu+72)==last_work->v67_dma);
                assert(strstr(fake.log,"stock signature restore deferred"));
            }
            if(fake.pins) {
                assert(fake.live && !fake.frees && last_work->retained && !last_work->quiesced);
                last_work->firmware_started=false;
                assert(!unlock_transaction_unwind(last_work));
                pci_dev_put(last_work->pdev);free(last_work);
            }
            assert(!fake.live && !fake.fw_live && dev.refs==1);
            assert(!fake.regions && !fake.group_live && list_empty(&dev.dev.devres_head));
            printf("AD102 full mode=%d result=%d PASS\n",cases[i],ret);
        }
        /* Dispatch the same published AD102-family payload through the exact
         * 0x2705 profile under mocked registers. This is not silicon proof. */
        ad102_setup(&w);dev.device=GU_AD103_RTX_4070_TI_SUPER;load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared));
        int ret=gpu_unlock_610_unlock(&dev,&shared);
        gpu_unlock_610_release_shared(&shared);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        puts("AD103 0x2705 profile dispatch under mocked registers PASS");
        {
        /* Synthetic AD103/A1 and 32 GiB geometry, not measured board values.
         * Exercise the high address bits and the same negative safety gates. */
        static const int cases4080[]={0,1,2,4,7,8,10};
        for (unsigned j=0;j<ARRAY_SIZE(cases4080);j++) {
        ad102_setup(&w);dev.device=GU_AD103_RTX_4080;regs[0]=0x193000a1;
        fake.ad_mode=cases4080[j];load_rom(argv[1]);
        regs[0x1183a4/4]=0x8000;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0xfffffff0;regs[0x1fa830/4]=0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;
        fake.fwsec_wpr_lo=(u32)((0x7ffe00000ull>>12)<<4);
        fake.fwsec_wpr_hi=(u32)((0x7ffee0000ull>>12)<<4);
        struct gpu_unlock_610_shared shared4080={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared4080));
        int ret4080=gpu_unlock_610_unlock(&dev,&shared4080);
        gpu_unlock_610_release_shared(&shared4080);
        assert((!ret4080)==(cases4080[j]==0) && fake.starts==1 && fake.gsp_starts==1);
        if(cases4080[j]==0)assert(fake.flrs==1);
        if(cases4080[j]==1 || cases4080[j]==2 || cases4080[j]==4 || cases4080[j]==10)
            assert(!fake.selector_writes && !fake.ss1_writes && !fake.flrs);
        if(fake.pins) {
            assert(fake.live && !fake.frees && last_work->retained);
            last_work->firmware_started=false;
            assert(!unlock_transaction_unwind(last_work));
            pci_dev_put(last_work->pdev);free(last_work);
        }
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        printf("AD103 0x2704 synthetic 32GiB mode=%d result=%d PASS\n",cases4080[j],ret4080);
        }
        }
        {
        /* Dispatch the same published AD102-family payload through the exact
         * 0x2786 profile under mocked registers. This is not silicon proof. */
        ad102_setup(&w);dev.device=GU_AD104_RTX_4070;load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared104={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared104));
        ret=gpu_unlock_610_unlock(&dev,&shared104);
        gpu_unlock_610_release_shared(&shared104);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        puts("AD104 0x2786 profile dispatch under mocked registers PASS");
        }
        const unsigned short ad106_ids[]={GU_AD106_RTX_4060_TI_16GB,GU_AD106_RTX_4060_TI,GU_AD106M_RTX_4070_MOBILE};
        for(unsigned i=0;i<ARRAY_SIZE(ad106_ids);i++) {
        /* Dispatch the same published AD102-family payload through the exact
         * 0x2805 and 0x2803 profiles under mocked registers. This is not silicon proof. */
        ad102_setup(&w);dev.device=ad106_ids[i];load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared106={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared106));
        ret=gpu_unlock_610_unlock(&dev,&shared106);
        gpu_unlock_610_release_shared(&shared106);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        printf("AD106 %#x profile dispatch under mocked registers PASS\n",ad106_ids[i]);
        }
        {
        /* Dispatch the same published AD102-family payload through the exact
         * 0x28e1 profile under mocked registers. This is not silicon proof. */
        ad102_setup(&w);dev.device=GU_AD107_RTX_4050_MOBILE;load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared107={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared107));
        ret=gpu_unlock_610_unlock(&dev,&shared107);
        gpu_unlock_610_release_shared(&shared107);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        puts("AD107 0x28e1 profile dispatch under mocked registers PASS");
        }
        {
        /* Dispatch the same published AD102-family payload through the exact
         * 0x28e0 profile under mocked registers. This is not silicon proof. */
        ad102_setup(&w);dev.device=GU_AD107_RTX_4060_MOBILE;load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared107_4060={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared107_4060));
        ret=gpu_unlock_610_unlock(&dev,&shared107_4060);
        gpu_unlock_610_release_shared(&shared107_4060);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        puts("AD107 0x28e0 profile dispatch under mocked registers PASS");
        }
        {
        /* Same AD102-family payload through the exact 0x2782 profile under
         * mocked registers. This is not silicon proof. */
        ad102_setup(&w);dev.device=GU_AD104_RTX_4070_TI;load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared104={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared104));
        ret=gpu_unlock_610_unlock(&dev,&shared104);
        gpu_unlock_610_release_shared(&shared104);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        puts("AD104 0x2782 profile dispatch under mocked registers PASS");
        }
        {
        /* Dispatch the same published AD102 payload through the exact 0x2689
         * cut-down AD102 profile under mocked registers. This is not silicon proof. */
        ad102_setup(&w);dev.device=GU_AD102_RTX_4070_TI_SUPER;load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared2689={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared2689));
        ret=gpu_unlock_610_unlock(&dev,&shared2689);
        gpu_unlock_610_release_shared(&shared2689);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        puts("AD102 0x2689 profile dispatch under mocked registers PASS");
        }
        {
        /* Same mocked dispatch for the AD103 RTX 4070 (0x2709). */
        ad102_setup(&w);dev.device=GU_AD103_RTX_4070;load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared2709={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared2709));
        ret=gpu_unlock_610_unlock(&dev,&shared2709);
        gpu_unlock_610_release_shared(&shared2709);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        puts("AD103 0x2709 profile dispatch under mocked registers PASS");
        }
        {
        /* Same mocked dispatch for the AD103 RTX 4080 SUPER (0x2702). */
        ad102_setup(&w);dev.device=GU_AD103_RTX_4080_SUPER;load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared2702={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared2702));
        ret=gpu_unlock_610_unlock(&dev,&shared2702);
        gpu_unlock_610_release_shared(&shared2702);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        puts("AD103 0x2702 profile dispatch under mocked registers PASS");
        }
        {
        /* Dispatch the same published AD102-family payload through the exact
         * 0x28a0 profile under mocked registers. This is not silicon proof. */
        ad102_setup(&w);dev.device=GU_AD107_RTX_4060_LAPTOP;load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared107={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared107));
        ret=gpu_unlock_610_unlock(&dev,&shared107);
        gpu_unlock_610_release_shared(&shared107);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        puts("AD107 0x28a0 profile dispatch under mocked registers PASS");
        }
        {
        /* Dispatch the same published AD102-family payload through the exact
         * 0x2757 profile under mocked registers. This is not silicon proof. */
        ad102_setup(&w);dev.device=GU_AD103M_RTX_4090_MOBILE;load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared103m={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared103m));
        ret=gpu_unlock_610_unlock(&dev,&shared103m);
        gpu_unlock_610_release_shared(&shared103m);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        puts("AD103M 0x2757 profile dispatch under mocked registers PASS");
        }
        {
        /* Dispatch the same published AD102-family payload through the exact
         * 0x2882 profile under mocked registers. This is not silicon proof. */
        ad102_setup(&w);dev.device=GU_AD107_RTX_4060;load_rom(argv[1]);
        regs[0x1183a4/4]=0x5ff4;regs[0x625f04/4]=1;regs[0x1fa7c8/4]=0x45cb8f;
        regs[0x1fa82c/4]=0x100000;regs[0x1fa830/4]=0x5cf3ff0;
        regs[(GU_FUSE_SEC2_UCODE1+2*4)/4]=1;
        fake.fwsec_wpr=1;fake.fwsec_wpr_lo=0x5ff2000;fake.fwsec_wpr_hi=0x5ff2e00;
        struct gpu_unlock_610_shared shared107={0};last_work=NULL;
        assert(!gpu_unlock_610_load_shared(&dev,&shared107));
        ret=gpu_unlock_610_unlock(&dev,&shared107);
        gpu_unlock_610_release_shared(&shared107);
        assert(!ret && fake.starts==1 && fake.gsp_starts==1 && fake.flrs==1);
        assert(!fake.live && !fake.fw_live && dev.refs==1);
        puts("AD107 0x2882 profile dispatch under mocked registers PASS");
        }
    }
    puts("AD102 marker, permission, HALT, readout, metadata and retention gates passed under mocked hardware");
}
