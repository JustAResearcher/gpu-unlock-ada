/* SPDX-License-Identifier: GPL-2.0-only */
/* Reuses the real-asset fixture, exercises admission independently of success. */
#define main transaction_fixture_main
#include "test_efi_transaction.c"
#undef main

static struct pci_dev Device;
static UINT64 Mask;
static VOID fresh(VOID) {
  test_entry_ss0=0x23613005;test_entry_ss1=1;setup(SUCCESS);
  Mask=0xffffffffffffULL;
  Device=(struct pci_dev){.vendor=0x10de,.device=0x2684,
    .dev={.dma_mask=&Mask,.coherent_dma_mask=0xfffffffffULL},.pm_cap=0x60,.refs=1};
}
static VOID rejected(const char *name,struct gpu_unlock_610_shared *shared) {
  CHECK(gpu_unlock_610_unlock(&Device,shared)!=0);
  CHECK(!Gu.started && !fake.mmio_writes && !fake.gsp_starts && !fake.sec2_starts && !fake.flrs);
  CHECK(!fake.live_maps && !fake.live_buffers && !Gu.allocations);
  CHECK(!fake.master_enables);printf("AD102 guard %s PASS\n",name);
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
  for(UINTN i=0;i<5;i++)free((VOID *)GuAssets[i].firmware.data);
  puts("17 admission cases PASS; all hardware operations simulated.");return 0;
}
