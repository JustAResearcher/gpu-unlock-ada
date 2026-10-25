/* SPDX-License-Identifier: GPL-2.0-only */
/* Reuses the real-asset fixture, exercises admission independently of success. */
#define main transaction_fixture_main
#include "test_efi_transaction.c"
#undef main
#include "rebar_cases4070s.h"

static struct pci_dev Device;
static UINT64 Mask;
static VOID fresh(VOID) {
  test_entry_ss0=0x62543025;test_entry_ss1=4;setup(SUCCESS);
  Mask=0xffffffffffffULL;
  Device=(struct pci_dev){.vendor=0x10de,.device=0x2783,
    .dev={.dma_mask=&Mask,.coherent_dma_mask=0xfffffffffULL},.pm_cap=0x60,.refs=1};
}
static VOID rejected(const char *name,struct gpu_unlock_610_shared *shared) {
  CHECK(gpu_unlock_610_unlock(&Device,shared)!=0);
  CHECK(!Gu.started && !fake.mmio_writes && !fake.gsp_starts && !fake.sec2_starts && !fake.flrs);
  CHECK(!fake.live_maps && !fake.live_buffers && !Gu.allocations);
  CHECK(!fake.master_enables && !fake.pci_writes);printf("AD104 guard %s PASS\n",name);
}
int main(int argc,char **argv) {
  CHECK(argc==3);load_fixture_assets(argv[1],argv[2]);
  for(unsigned i=0;i<5;i++) {
    fresh();struct gpu_unlock_610_shared shared={0};
    CHECK(!gpu_unlock_610_load_shared(&Device,&shared));
    UINT8 *p=(UINT8 *)GuAssets[i].firmware.data;UINTN n=GuAssets[i].firmware.size;
    p[n-1]^=1;rejected("mutated asset after preload",&shared);p[n-1]^=1;
    gpu_unlock_610_release_shared(&shared);dispose_host_fixture();
  }
  for(unsigned i=0;i<10;i++) {
    fresh();struct gpu_unlock_610_shared shared={0};CHECK(!gpu_unlock_610_load_shared(&Device,&shared));
    switch(i) {
      case 0:config[0]^=1;break;
      case 1:config[0x2c]^=1;break;
      case 2:config[8]^=1;break;
      case 3:config[4]|=4;break;
      case 4:config[0x10]^=0x10;break;
      case 5:Gu.owned=FALSE;break;
      case 6:Gu.display_check=NULL;break;
      case 7:registers[0x823818/4]^=1;break;
      case 8:Device.device=0x2705;break;
      case 9:Gu.transaction_attempted=TRUE;break;
    }
    rejected("identity/cold state/ownership/retry",&shared);
    gpu_unlock_610_release_shared(&shared);dispose_host_fixture();
  }
  for(unsigned i=0;i<2;i++) {
    fresh();struct gpu_unlock_610_shared shared={0};CHECK(!gpu_unlock_610_load_shared(&Device,&shared));
    if(!i)((UINT8 *)registers)[0x3fffff]^=1;else registers[0x8241e0/4]=3;
    rejected(i?"unsupported FWSEC fuse version":"changed physical ROM",&shared);
    CHECK(i || Gu.transaction_attempted);
    UINTN writes=fake.mmio_writes,maps=fake.maps;
    CHECK(gpu_unlock_610_unlock(&Device,&shared)!=0 && fake.maps==maps && fake.mmio_writes==writes);
    gpu_unlock_610_release_shared(&shared);dispose_host_fixture();
  }
  /* Keep the original 17 fault cases above; exercise new identity/SS1 boundary. */
  for(unsigned i=0;i<6;i++) {
    fresh();struct gpu_unlock_610_shared shared={0};CHECK(!gpu_unlock_610_load_shared(&Device,&shared));
    switch(i) {
      case 0:Device.device=0x2782;break;
      case 1: {UINT32 old=0x278210de;memcpy(config,&old,4);break;}
      case 2: {UINT32 old=0x51321462;memcpy(config+0x2c,&old,4);break;}
      case 3:registers[SS1/4]=1;break;
      case 4:Gu.armed=FALSE;break;
      case 5:Gu.display_ok=FALSE;break;
    }
    rejected("previous board / SS1 / authorization",&shared);
    gpu_unlock_610_release_shared(&shared);dispose_host_fixture();
  }
  fresh();Device.device=0x2782;struct gpu_unlock_610_shared old_shared={0};
  CHECK(gpu_unlock_610_load_shared(&Device,&old_shared)!=0 && !old_shared.gsp);
  CHECK(!fake.pci_writes && !fake.mmio_writes && !fake.maps);dispose_host_fixture();
  for(UINTN i=0;i<sizeof(rebar_invalid)/sizeof(rebar_invalid[0]);i++) {
    fresh();struct gpu_unlock_610_shared shared={0};CHECK(!gpu_unlock_610_load_shared(&Device,&shared));
    UINT32 value=le32(config+rebar_invalid[i][0])^rebar_invalid[i][1];memcpy(config+rebar_invalid[i][0],&value,4);
    rejected("ReBAR changed after Prepare",&shared);
    gpu_unlock_610_release_shared(&shared);dispose_host_fixture();
  }
  for(UINTN i=0;i<7;i++) {
    fresh();struct gpu_unlock_610_shared shared={0};CHECK(!gpu_unlock_610_load_shared(&Device,&shared));
    fake.entry_read_error=rebar_offsets[i];rejected("ReBAR read error after Prepare",&shared);
    CHECK(fake.config_error_injected);gpu_unlock_610_release_shared(&shared);dispose_host_fixture();
  }
  for(UINTN i=0;i<5;i++)free((VOID *)GuAssets[i].firmware.data);
  puts("64 admission cases PASS (original 17 retained); all hardware operations simulated.");return 0;
}
