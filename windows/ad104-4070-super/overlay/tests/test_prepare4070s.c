/* SPDX-License-Identifier: GPL-2.0-only */
/* Exact captured board/PROM; no host hardware or firmware execution. */
#define main transaction_fixture_main
#include "test_efi_transaction.c"
#undef main
#include "rebar_cases4070s.h"

static VOID readonly_admission(VOID) {
  CHECK(!fake.pci_writes && !fake.mmio_writes && !fake.maps && !fake.master_enables);
  CHECK(!Gu.started && !Gu.allocations && !fake.pool_live && !fake.flrs);
}
static VOID rejected_prepare(VOID) {
  UINT32 a,b;
  CHECK(EFI_ERROR(GuAd104Prepare()));readonly_admission();
  CHECK(!Gu.profile_verified && !Gu.prom_verified && !GuGetEntrySelectors(&a,&b));
  dispose_host_fixture();
}
static VOID pins(VOID) {
  struct gpu_unlock_fwsec fw;
  CHECK(GuAd104AssetsValid());
  CHECK(GuAd104PromMatches(prom,sizeof(prom)));
  CHECK(!gpu_unlock_vbios_fwsec(prom,sizeof(prom),&fw));
  CHECK(fw.image_off==0x45b44 && fw.image_size==0x10500 && fw.imem_pa==0 &&
    fw.imem_size==0xf700 && fw.imem_va==0 && fw.dmem_pa==0 && fw.data_off==0xf700 &&
    fw.dmem_size==0xd80 && fw.pkc_off==0xb24 && fw.interface_off==0x1c && fw.ucode_id==9 &&
    fw.engine_mask==0x400 && fw.sig_count==2 && fw.sig_versions==3 &&
    fw.sig_off==0x45844 && fw.sig_bytes==768);
  CHECK(GuAd104FwsecMatches(&fw));
  /* Every field is pinned. Never accept a near match or the former geometry. */
#define MUTATE_FW(field) do {struct gpu_unlock_fwsec bad=fw;bad.field^=1;CHECK(!GuAd104FwsecMatches(&bad));} while(0)
  MUTATE_FW(image_off);MUTATE_FW(image_size);MUTATE_FW(imem_pa);MUTATE_FW(imem_size);
  MUTATE_FW(imem_va);MUTATE_FW(dmem_pa);MUTATE_FW(data_off);MUTATE_FW(dmem_size);
  MUTATE_FW(pkc_off);MUTATE_FW(interface_off);MUTATE_FW(ucode_id);MUTATE_FW(engine_mask);
  MUTATE_FW(sig_count);MUTATE_FW(sig_versions);MUTATE_FW(sig_off);MUTATE_FW(sig_bytes);
#undef MUTATE_FW
  struct gpu_unlock_fwsec old=fw;
  old.image_off=0x45488;old.image_size=0x10300;old.imem_size=old.data_off=0xf500;old.sig_off=0x45188;
  CHECK(!GuAd104FwsecMatches(&old) && !GuAd104FwsecMatches(NULL));
  prom[sizeof(prom)-1]^=1;CHECK(!GuAd104PromMatches(prom,sizeof(prom)));prom[sizeof(prom)-1]^=1;
  CHECK(!GuAd104PromMatches(prom,sizeof(prom)-1));
  puts("4070S real PROM/assets/FWSEC positive + 20 pin rejection checks PASS");
}
static VOID positive(VOID) {
  static const UINT32 entries[]={0x62543025,0x23613005,0x23603005};
  for(UINTN i=0;i<sizeof(entries)/sizeof(entries[0]);i++) {
    test_entry_ss0=entries[i];test_entry_ss1=4;setup_unprepared(SUCCESS);
    CHECK(le32(config)==0x278310de && le32(config+0x2c)==0x413b1458);
    CHECK(registers[(SEC2+0x100)/4]==0xbadf5620); /* Diagnostic only at cold admission. */
    CHECK(GuAd104Prepare()==EFI_SUCCESS);readonly_admission();
    CHECK(Gu.profile_verified && Gu.prom_verified && GuAd104ColdEntry());
    UINT32 a,b;CHECK(GuGetEntrySelectors(&a,&b) && a==entries[i] && b==4);
    CHECK(!GuLatchEntrySelectors(a^1,b));
    CHECK(GuGetEntrySelectors(&a,&b) && a==entries[i] && b==4);
    CHECK(EFI_ERROR(GuAd104Prepare()));readonly_admission();
    registers[SS0/4]^=1;CHECK(!GuAd104ColdEntry());registers[SS0/4]^=1;
    registers[SS1/4]=1;CHECK(!GuAd104ColdEntry());registers[SS1/4]=4;
    CHECK(GuAd104ColdEntry());
    for(UINTN j=0;j<7;j++)CHECK(fake.rebar_reads[j]>=2);
    dispose_host_fixture();
  }
  puts("4070S read-only Prepare: captured SS0 plus two dynamic latches, SS1=4, cold SEC2 inaccessible PASS");
}
static VOID rejections(VOID) {
  static const UINT32 cold_offsets[]={0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,
    0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,0x823814,0x823818,0x1183a4,0x625f04,
    0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0};
  static const UINT32 invalid_ss0[]={0xffffffff,0xbadf0000,0x88888888};
  UINTN cases=0;
  test_entry_ss0=0x62543025;test_entry_ss1=4;
  for(UINTN i=0;i<sizeof(cold_offsets)/sizeof(cold_offsets[0]);i++) {
    setup_unprepared(SUCCESS);registers[cold_offsets[i]/4]^=1;rejected_prepare();cases++;
  }
  for(UINT32 v=0;v<=16;v++)if(v!=4) {
    setup_unprepared(SUCCESS);registers[SS1/4]=v;rejected_prepare();cases++;
  }
  for(UINTN i=0;i<sizeof(invalid_ss0)/sizeof(invalid_ss0[0]);i++) {
    setup_unprepared(SUCCESS);registers[SS0/4]=invalid_ss0[i];rejected_prepare();cases++;
  }
  for(UINTN i=0;i<sizeof(rebar_invalid)/sizeof(rebar_invalid[0]);i++) {
    setup_unprepared(SUCCESS);UINT32 v=le32(config+rebar_invalid[i][0])^rebar_invalid[i][1];
    memcpy(config+rebar_invalid[i][0],&v,4);rejected_prepare();cases++;
  }
  for(UINTN i=0;i<7;i++) {
    setup_unprepared(SUCCESS);fake.entry_read_error=rebar_offsets[i];rejected_prepare();
    CHECK(fake.config_error_injected);cases++;
  }
  for(UINTN i=0;i<9;i++) {
    setup_unprepared(SUCCESS);
    switch(i) {
      case 0: {UINT32 old=0x278210de;memcpy(config,&old,4);break;}
      case 1: {UINT32 old=0x51321462;memcpy(config+0x2c,&old,4);break;}
      case 2:config[8]^=1;break;
      case 3:config[4]|=4;break;
      case 4:config[0x10]^=0x10;break;
      case 5:Gu.bar_size-=4096;break;
      case 6:((UINT8 *)registers)[0x3fffff]^=1;break;
      case 7:registers[0x8241e0/4]=3;break;
      case 8:Gu.bar_base+=4096;break;
    }
    rejected_prepare();cases++;
  }
  printf("4070S Prepare %zu rejection cases PASS; no GPU/PCI mutation\n",cases);
}
int main(int argc,char **argv) {
  CHECK(argc==4);load_fixture_assets(argv[1],argv[2]);
  if(!strcmp(argv[3],"pins"))pins();
  else if(!strcmp(argv[3],"positive"))positive();
  else {CHECK(!strcmp(argv[3],"rejections"));rejections();}
  for(UINTN i=0;i<5;i++)free((VOID *)GuAssets[i].firmware.data);
  return 0;
}
