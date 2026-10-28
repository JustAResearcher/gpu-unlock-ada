/* SPDX-License-Identifier: GPL-2.0-only */
/* Exact captured RTX 4090 FE on MSI MAG B650 TOMAHAWK WIFI. A matched profile is not hardware qualification. */
#include "profile.h"
#include "checks.h"
#include "sha256.h"
#include "compat/compat.h"

static const struct {
  const char *name,*hash;
  UINTN bytes;
} Pins[5]={
 {"gpu-unlock/610.43.03/booter_load_ad102.bin","6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49",57672},
 {"gpu-unlock/610.43.03/v67-ad102-610.43.03-four-plm.bin","44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb",64000},
 {"gpu-unlock/610.43.03/gsp_ga10x.bin","572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a",84277400},
 {"gpu-unlock/610.43.03/gsp_boot_ad102.bin","45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0",36864},
 {"gpu-unlock/610.43.03/gsp_boot_desc_ad102.bin","83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42",84}
};
BOOLEAN GuProfileAssetsValid(VOID) {
  for(UINTN i=0;i<5;i++)
    if(!GuAssets[i].name || strcmp(GuAssets[i].name,Pins[i].name) ||
       GuAssets[i].firmware.size!=Pins[i].bytes ||
       !GuHashMatches(GuAssets[i].firmware.data,Pins[i].bytes,Pins[i].hash))return FALSE;
  return TRUE;
}
BOOLEAN GuProfilePromMatches(const VOID *Data,UINTN Bytes) {
  return Bytes==1048576 && GuHashMatches(Data,Bytes,
    "c03931779344e3b73002f6f8da2870327ac24242a6a8c76f9a42f11f1ca9f780");
}
BOOLEAN GuProfileFwsecMatches(const struct gpu_unlock_fwsec *f) {
  return f && f->image_off==0x45898 && f->image_size==0x10500 &&
    f->imem_pa==0 && f->imem_size==0xf700 && f->imem_va==0 &&
    f->dmem_pa==0 && f->data_off==0xf700 && f->dmem_size==0xd80 &&
    f->pkc_off==0xb24 && f->interface_off==0x1c && f->ucode_id==9 &&
    f->engine_mask==0x400 && f->sig_count==2 && f->sig_versions==3 &&
    f->sig_off==0x45598 && f->sig_bytes==768;
}

static BOOLEAN Config(VOID) {
  UINT32 c[64];UINTN s,b,d,f;GU_CAPS caps;
  if(!Gu.pci || !Gu.pci->GetLocation || !Gu.pci->Pci.Read ||
     Gu.bar_base!=GU_BAR0_BASE || Gu.bar_size!=0x1000000 ||
     EFI_ERROR(Gu.pci->GetLocation(Gu.pci,&s,&b,&d,&f)))return FALSE;
  for(UINTN i=0;i<64;i++)
    if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)i*4,&c[i])))return FALSE;
  if(!GuCheckConfig(c,s,b,d,f,&caps) || c[2]!=0x030000a1 ||
     c[4]!=GU_BAR0_BASE || c[5]!=0xc || c[6]!=0xf0 || c[7]!=0xc ||
     c[8]!=0xf8 || c[9]!=0xf001)return FALSE;
  return caps.pm==Gu.pm_cap && caps.pcie==Gu.pcie_cap &&
    caps.pcie_version==Gu.pcie_version && caps.msi==Gu.msi_cap && caps.msix==Gu.msix_cap;
}
static BOOLEAN Registers(UINT32 *Ss0,UINT32 *Ss1) {
  static const UINT32 offsets[]={0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,
    0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,0x823814,0x823818,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0};
  static const UINT32 values[]={0x192000a1,0x8b8f,0x3ff,0x10,0x1ffffe00,0,
    0x4cb8f,0x4cb8f,0xffffff0f,0xffffff8f,0x233,0x410080,0x5ff4,1,0x45cb8f,0x100000,0x5cf3ff0,1,1};
  for(UINTN i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++)
    if(GuRead32(offsets[i])!=values[i] || Gu.fault)return FALSE;
  *Ss0=GuRead32(0x82381c);*Ss1=GuRead32(0x823820);
  return !Gu.fault && *Ss0!=0xffffffff && (*Ss0&0xffff0000)!=0xbadf0000 &&
    *Ss0!=0x88888888 && *Ss1==1;
}
BOOLEAN GuProfileColdEntry(VOID) {
  UINT32 ss0,ss1;
  return !Gu.started && !Gu.fault && !Gu.retained && Config() &&
    Registers(&ss0,&ss1) && GuEntrySelectorsMatch(ss0,ss1);
}
EFI_STATUS GuProfilePrepare(VOID) {
  UINT32 ss0,ss1,after0,after1;VOID *prom=NULL;struct gpu_unlock_fwsec fw;
  if(Gu.started || Gu.fault || Gu.retained || Gu.allocations || Gu.profile_verified ||
     Gu.prom_verified || !Config() || !Registers(&ss0,&ss1))return EFI_NOT_READY;
  if(!GuProfileAssetsValid())return EFI_CRC_ERROR;
  prom=GuAlloc(1048576);if(!prom)return EFI_OUT_OF_RESOURCES;
  GuCopyFromIo(prom,(VOID *)(UINTN)(Gu.bar_base+0x300000),1048576);
  BOOLEAN valid=!Gu.fault && GuProfilePromMatches(prom,1048576) &&
    !gpu_unlock_vbios_fwsec(prom,1048576,&fw) && GuProfileFwsecMatches(&fw) &&
    Config() && Registers(&after0,&after1) && ss0==after0 && ss1==after1;
  GuFree(prom);
  if(!valid)return EFI_COMPROMISED_DATA;
  Gu.prom_verified=TRUE;
  if(!GuLatchEntrySelectors(ss0,ss1)){Gu.prom_verified=FALSE;return EFI_NOT_READY;}
  Gu.profile_verified=TRUE;return EFI_SUCCESS;
}
