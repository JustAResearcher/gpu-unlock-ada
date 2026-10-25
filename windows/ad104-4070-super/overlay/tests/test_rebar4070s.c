/* SPDX-License-Identifier: GPL-2.0-only */
/* Isolate real port PCI logic from the separate exact-board/PROM admission.
 * Only this seam follows current GU_BOARD_ID/SUBSYSTEM, so upstream RED can
 * expose missing ReBAR restoration even before 2783 admission exists. */
#define main transaction_fixture_main
#include "test_efi_transaction.c"
#undef main
#include "rebar_cases4070s.h"

static VOID seam(enum scenario mode) {
  test_entry_ss0=0x62543025;test_entry_ss1=4;setup_unprepared(mode);
  UINT32 id=GU_BOARD_ID,subsystem=GU_BOARD_SUBSYSTEM;
  memcpy(config,&id,4);memcpy(config+0x2c,&subsystem,4);
  fake.isolated_rebar=TRUE;Gu.profile_verified=Gu.prom_verified=TRUE;
  CHECK(GuLatchEntrySelectors(test_entry_ss0,4));
}
static VOID reset_once(VOID) {
  CHECK(GuSavePci()==EFI_SUCCESS && Gu.saved);
  CHECK(!fake.pci_writes && !fake.mmio_writes);
  GuStarted();GuWrite32(SS0,0x88888888);
  CHECK(GuSelectorPhase(GU_SELECTOR_PHASE_SS0_WRITTEN));
  CHECK(GuSelectorPhase(GU_SELECTOR_PHASE_SS1_WRITE));GuWrite32(SS1,8);
  CHECK(GuSelectorPhase(GU_SELECTOR_PHASE_TARGET));
  CHECK(GuFlr()==EFI_SUCCESS && Gu.flrs==1 && fake.flrs==1);
  CHECK(fake.delay_since_flr>=100000 && !(config[4]&4));
  for(UINTN i=0;i<3;i++) {
    UINT32 expected=rebar_values[i]&~0x3f00U;
    if(fake.restore_drift_offset==rebar_controls[i])expected^=fake.restore_drift_mask;
    CHECK(le32(config+rebar_controls[i])==expected);
  }
}
static VOID save_checks(BOOLEAN read_error) {
  UINTN count=read_error?7:sizeof(rebar_invalid)/sizeof(rebar_invalid[0]);
  for(UINTN i=0;i<count;i++) {
    seam(SUCCESS);
    if(read_error)fake.entry_read_error=rebar_offsets[i];
    else {UINT32 v=le32(config+rebar_invalid[i][0])^rebar_invalid[i][1];memcpy(config+rebar_invalid[i][0],&v,4);}
    CHECK(EFI_ERROR(GuSavePci()) && !Gu.saved);
    if(read_error)CHECK(fake.config_error_injected);
    CHECK(!fake.pci_writes && !fake.mmio_writes && !fake.maps && !fake.flrs);
    dispose_host_fixture();
  }
  seam(SUCCESS);CHECK(GuSavePci()==EFI_SUCCESS && Gu.saved);
  for(UINTN i=0;i<7;i++)CHECK(fake.rebar_reads[i]);
  CHECK(!fake.pci_writes && !fake.mmio_writes);dispose_host_fixture();
  printf("ReBAR PCI save: %zu %s rejections plus captured snapshot PASS\n",count,read_error?"read-error":"structure/size");
}
static VOID restore_positive(UINT32 size_drift_offset) {
  seam(SUCCESS);
  if(size_drift_offset) {
    fake.restore_drift_offset=size_drift_offset;fake.restore_drift_mask=0x2000;
  }
  reset_once();
  CHECK(GuRestorePci()==EFI_SUCCESS && !Gu.fault);
  CHECK(fake.rebar_verified==7 && fake.bar_writes==6 && fake.decode_enables==1);
  for(UINTN i=0;i<7;i++)CHECK(le32(config+rebar_offsets[i])==rebar_words[i]);
  for(UINTN i=0;i<3;i++)CHECK(fake.rebar_writes[i]==1);
  CHECK(!memcmp(config+0x10,CapturedConfig4070S+0x10,24) && !(config[4]&4));
  CHECK(EFI_ERROR(GuFlr()) && fake.flrs==1);GuQuiesced();CHECK(GuSafeToReturn());
  dispose_host_fixture();
  puts("ReBAR original sizes 16MiB/16GiB/32MiB restored before BARs/decode; one real port FLR PASS");
}
static VOID transient_bme_oracle(VOID) {
  seam(SUCCESS);reset_once();CHECK(config[4]==3);
  /* Deliberately violate the trace oracle. The runner must observe the BME
     assertion and exit 1; accepting both writes and ending at zero is a failure.
     Call the fake EFI endpoint directly so a future production guard cannot
     conceal a regression in this test oracle. */
  UINT16 command=7;
  CHECK(write_config(&pci,EfiPciIoWidthUint16,4,1,&command)==EFI_SUCCESS);
  command=0;CHECK(write_config(&pci,EfiPciIoWidthUint16,4,1,&command)==EFI_SUCCESS);
  CHECK(config[4]==0);
  puts("ORACLE ESCAPED: post-FLR Command 3 -> 7 -> 0 accepted despite transient BME");
  dispose_host_fixture();
}
static VOID restore_faults(VOID) {
  for(int mode=REBAR_WRITE_0;mode<=REBAR_DECODE_STUCK;mode++) {
    seam((enum scenario)mode);reset_once();
    CHECK(EFI_ERROR(GuRestorePci()));
    CHECK(!fake.bar_writes && !fake.decode_enables && !Gu.quiesced && !GuSafeToReturn());
    if(mode!=REBAR_DECODE_STUCK)CHECK(!(config[4]&7));
    else CHECK(!(config[4]&4));
    if(mode>=REBAR_DRIFT_HEADER)for(UINTN i=0;i<3;i++)CHECK(!fake.rebar_writes[i]);
    if(mode<=REBAR_READBACK_ERROR_2 || (mode>=REBAR_READ_ERROR_HEADER && mode<=REBAR_READ_ERROR_CTL2))CHECK(fake.config_error_injected);
    CHECK(EFI_ERROR(GuFlr()) && fake.flrs==1);dispose_host_fixture();
  }
  puts("ReBAR restore: each control write/readback/error, each structure/read error, decode verification PASS");
}
static VOID restore_structure(VOID) {
  UINTN cases=0;
  for(UINTN i=0;i<sizeof(rebar_invalid)/sizeof(rebar_invalid[0]);i++) {
    UINT32 offset=rebar_invalid[i][0],mask=rebar_invalid[i][1];
    if((offset==0xbb8 || offset==0xbc0 || offset==0xbc8) && (mask&0x3f00))continue;
    seam(SUCCESS);fake.restore_drift_offset=offset;fake.restore_drift_mask=mask;reset_once();
    CHECK(EFI_ERROR(GuRestorePci()) && !(config[4]&7));
    CHECK(!fake.bar_writes && !fake.decode_enables && !GuSafeToReturn());
    for(UINTN j=0;j<3;j++)CHECK(!fake.rebar_writes[j]);
    dispose_host_fixture();cases++;
  }
  printf("ReBAR restore %zu header/capability/index/count/reserved drift cases PASS\n",cases);
}
static VOID read_boundary(VOID) {
  seam(SUCCESS);
  for(UINTN i=0;i<7;i++) {
    UINT32 value=0;CHECK(GuConfigRead(EfiPciIoWidthUint32,rebar_offsets[i],&value)==EFI_SUCCESS);
    CHECK(value==rebar_words[i]);
  }
  UINT64 value=0;UINTN count=0;
  for(UINT32 offset=256;offset<=4096;offset++)for(UINT32 width=0;width<=EfiPciIoWidthMaximum;width++) {
    if(width==EfiPciIoWidthUint32 && offset>=0xbb0 && offset<=0xbc8 && !(offset&3))continue;
    Gu.fault=FALSE;UINTN before=fake.config_reads;
    CHECK(EFI_ERROR(GuConfigRead((EFI_PCI_IO_PROTOCOL_WIDTH)width,offset,&value)));
    CHECK(before==fake.config_reads);count++;
  }
  static const UINT32 boundary[][2]={{EfiPciIoWidthUint32,253},{EfiPciIoWidthUint16,255},{EfiPciIoWidthUint32,0xffffffff}};
  for(UINTN i=0;i<3;i++) {
    Gu.fault=FALSE;UINTN before=fake.config_reads;
    CHECK(EFI_ERROR(GuConfigRead((EFI_PCI_IO_PROTOCOL_WIDTH)boundary[i][0],boundary[i][1],&value)));
    CHECK(before==fake.config_reads);
  }
  Gu.fault=FALSE;CHECK(GuConfigRead(EfiPciIoWidthUint32,252,&value)==EFI_SUCCESS);
  CHECK(GuConfigRead(EfiPciIoWidthUint16,254,&value)==EFI_SUCCESS);
  CHECK(GuConfigRead(EfiPciIoWidthUint8,255,&value)==EFI_SUCCESS);
  dispose_host_fixture();printf("ReBAR read whitelist: 7 legal + %zu illegal extended + standard boundaries PASS\n",count);
}
static VOID denied_write(EFI_PCI_IO_PROTOCOL_WIDTH width,UINT32 offset,UINT32 value) {
  UINTN before=fake.pci_writes;
  CHECK(EFI_ERROR(GuConfigWrite(width,offset,&value)) && before==fake.pci_writes);
}
static VOID write_boundary(VOID) {
  /* These invalid phases cannot reach even a valid control/value callback. */
  for(UINTN phase=0;phase<3;phase++) {
    seam(SUCCESS);
    if(phase==1)CHECK(GuSavePci()==EFI_SUCCESS);
    if(phase==2)reset_once();
    denied_write(EfiPciIoWidthUint32,0xbb8,0x460);dispose_host_fixture();
  }
  seam(SUCCESS);reset_once();
  UINT16 command=0;CHECK(GuConfigWrite(EfiPciIoWidthUint16,4,&command)==EFI_SUCCESS);
  CHECK(GuConfigRead(EfiPciIoWidthUint16,4,&command)==EFI_SUCCESS && !command);
  UINTN count=0;
  for(UINT32 offset=256;offset<=4096;offset++)for(UINT32 width=0;width<=EfiPciIoWidthMaximum;width++) {
    if(width==EfiPciIoWidthUint32 && (offset==0xbb8 || offset==0xbc0 || offset==0xbc8))continue;
    Gu.fault=FALSE;
    /* A valid saved value cannot turn another offset or width into a legal write. */
    denied_write((EFI_PCI_IO_PROTOCOL_WIDTH)width,offset,0x460);count++;
  }
  for(UINTN i=0;i<3;i++) {
    Gu.fault=FALSE;denied_write(EfiPciIoWidthUint32,rebar_controls[i],rebar_values[i]^0x100);count++;
  }
  for(UINTN guard=0;guard<8;guard++) {
    Gu.fault=FALSE;Gu.saved=TRUE;Gu.flrs=1;Gu.owned=Gu.armed=Gu.display_ok=Gu.profile_verified=TRUE;
    config[4]=0;
    switch(guard) {
      case 0:Gu.saved=FALSE;break;
      case 1:Gu.flrs=0;break;
      case 2:Gu.flrs=2;break;
      case 3:Gu.owned=FALSE;break;
      case 4:Gu.armed=FALSE;break;
      case 5:Gu.display_ok=FALSE;break;
      case 6:Gu.profile_verified=FALSE;break;
      case 7:config[4]=4;break;
    }
    denied_write(EfiPciIoWidthUint32,0xbb8,0x460);
  }
  dispose_host_fixture();printf("ReBAR write whitelist: %zu illegal offset/width/value + 11 phase/authority checks PASS\n",count);
}
int main(int argc,char **argv) {
  CHECK(argc==2);
  if(!strcmp(argv[1],"save"))save_checks(FALSE);
  else if(!strcmp(argv[1],"save-read-errors"))save_checks(TRUE);
  else if(!strcmp(argv[1],"restore"))restore_positive(0);
  else if(!strcmp(argv[1],"restore-size-bit13")) {
    for(UINTN i=0;i<3;i++)restore_positive(rebar_controls[i]);
  }
  else if(!strcmp(argv[1],"oracle-transient-bme"))transient_bme_oracle();
  else if(!strcmp(argv[1],"restore-faults"))restore_faults();
  else if(!strcmp(argv[1],"restore-structure"))restore_structure();
  else if(!strcmp(argv[1],"read-boundary"))read_boundary();
  else {CHECK(!strcmp(argv[1],"write-boundary"));write_boundary();}
  return 0;
}
