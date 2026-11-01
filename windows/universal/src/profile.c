/* SPDX-License-Identifier: GPL-2.0-only */
/* Exact captured MSI AD104. A matched profile is not hardware qualification. */
#include "profile.h"
#include "checks.h"
#include "sha256.h"
#include "compat/compat.h"

static const struct {
  const char *name,*hash;
  UINTN bytes;
} Pins[9]={
  {"gsp_ga10x.bin","572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a",84277400},
  {"booter_load_ad102.bin","6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49",57672},
  {"v67-ad102-610.43.03-four-plm.bin","44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb",64000},
  {"gsp_boot_ad102.bin","45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0",36864},
  {"gsp_boot_desc_ad102.bin","83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42",84},
  {"booter_load_ga102.bin","056cbc064a888f80717726c54d7f64fcd68044d123870c95a74f37f4975dcaa4",61256},
  {"v67-ga102-610.43.03.bin","2a0aa1f86d0b6afff5214d641d87d46dc98ef06ed908ff05d0ddbad5ffb7e197",64000},
  {"gsp_boot_ga102.bin","3df651acb174fd5f5561a1996965b192f50c5d763168446ed0d4ead19d895768",24576},
  {"gsp_boot_desc_ga102.bin","cc5a3fc1f4c47f2fe107b1b68f0176d8a448e6dec84b575e5560ccef5262bf15",84}
};
BOOLEAN GuAssetsValid(VOID) {
  if (GuAssetCount == 0) return FALSE;
  for(UINTN i=0;i<GuAssetCount;i++) {
    BOOLEAN match = FALSE;
    const char *a_base = GuAssets[i].name;
    for (const char *p = GuAssets[i].name; *p; p++) {
      if (*p == '/' || *p == '\\') a_base = p + 1;
    }
    for(UINTN p=0;p<sizeof(Pins)/sizeof(Pins[0]);p++) {
      const char *pin_base = Pins[p].name;
      for (const char *k = Pins[p].name; *k; k++) {
        if (*k == '/' || *k == '\\') pin_base = k + 1;
      }
      if(!strcmp(a_base, pin_base)) {
        if(GuAssets[i].firmware.size==Pins[p].bytes &&
           GuHashMatches(GuAssets[i].firmware.data, Pins[p].bytes, Pins[p].hash)) {
          match = TRUE;
        }
        break;
      }
    }
    if(!match) return FALSE;
  }
  return TRUE;
}
BOOLEAN GuAd104AssetsValid(VOID) {
  return GuAssetsValid();
}
BOOLEAN GuAd104PromMatches(const VOID *Data,UINTN Bytes) {
  if (!Data || Bytes < 626176) return FALSE;
  if (GuHashMatches(Data, 626176, "35a6c1ad3225b1266cfacaf6ea014f137f32c54888eb676e230a33a8ac9d19c2")) return TRUE;
  if (Bytes == 1048576) {
    if (GuHashMatches(Data, Bytes, "4891c171e39da7839677b115c7acd6df77720a53b23c7d8ad5226d2c6f42ba05") ||
        GuHashMatches(Data, Bytes, "2b6a8a3d2d862476bdcdf6a2e382cdd5293d60127a7dfb519515bc496d945fff") ||
        GuHashMatches(Data, Bytes, "b276ec756a84882236e016e7b4b3a57e334000b2a3c6948a73771662092780d9") ||
        GuHashMatches(Data, Bytes, "e2f145d5cfe262d560f301696e74db1b7e6dccc1aada87b148cf04b516ed79b2"))
      return TRUE;
  }
  return FALSE;
}
BOOLEAN GuAd104FwsecMatches(const struct gpu_unlock_fwsec *f) {
  return f && f->image_off==0x45488 && f->image_size==0x10300 &&
    f->imem_pa==0 && f->imem_size==0xf500 && f->imem_va==0 &&
    f->dmem_pa==0 && f->data_off==0xf500 && f->dmem_size==0xd80 &&
    f->pkc_off==0xb24 && f->interface_off==0x1c && f->ucode_id==9 &&
    f->engine_mask==0x400 && f->sig_count==2 && f->sig_versions==3 &&
    f->sig_off==0x45188 && f->sig_bytes==768;
}

static BOOLEAN Config(VOID) {
  UINT32 c[64];UINTN s,b,d,f;GU_CAPS caps;
  if(!Gu.pci || !Gu.pci->GetLocation || !Gu.pci->Pci.Read ||
     Gu.bar_base!=GU_BAR0_BASE || Gu.bar_size!=0x1000000 ||
     EFI_ERROR(Gu.pci->GetLocation(Gu.pci,&s,&b,&d,&f))) {
    GuLog("Config fail: PCI protocol/location or BAR invalid base=%#llx\n", (unsigned long long)Gu.bar_base);
    return FALSE;
  }
  for(UINTN i=0;i<64;i++)
    if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)i*4,&c[i]))) {
      GuLog("Config fail: read offset %#x failed\n", (unsigned)(i*4));
      return FALSE;
    }
  if(!GuCheckConfig(c,s,b,d,f,&caps)) {
    GuLog("Config fail: GuCheckConfig failed (c[0]=%#x c[11]=%#x bus=%zu)\n", c[0], c[11], b);
    return FALSE;
  }
  if(c[2]!=0x030000a1) {
    GuLog("Config fail: c[2]=%#x != 0x030000a1\n", c[2]);
    return FALSE;
  }
  if(c[4]!=GU_BAR0_BASE) {
    GuLog("Config fail: c[4]=%#x != GU_BAR0_BASE (%#llx)\n", c[4], (unsigned long long)GU_BAR0_BASE);
    return FALSE;
  }
  if((c[5]&0xf)!=0xc || (c[7]&0xf)!=0xc || (c[9]&1)!=1) {
    GuLog("Config fail: BAR types mismatch c[5]=%#x c[7]=%#x c[9]=%#x\n", c[5], c[7], c[9]);
    return FALSE;
  }
  BOOLEAN caps_ok = caps.pm==Gu.pm_cap && caps.pcie==Gu.pcie_cap &&
    caps.pcie_version==Gu.pcie_version && caps.msi==Gu.msi_cap && caps.msix==Gu.msix_cap;
  if (!caps_ok) {
    GuLog("Config fail: caps drift\n");
    return FALSE;
  }
  return TRUE;
}
static BOOLEAN Registers(UINT32 *Ss0,UINT32 *Ss1) {
  static const UINT32 offsets[]={0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,
    0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,0x823814,0x823818,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0};
  static const UINT32 values[]={0x194000a1,0x8b8f,0x3ff,0x10,0x1ffffe00,0,
    0x4cb8f,0x4cb8f,0xffffff0f,0xffffff8f,0x233,0x410080,0x2ffa,1,0x45cb8f,0x100000,0x2ef9ff0,1,1};
  for(UINTN i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
    UINT32 val = GuRead32(offsets[i]);
    if(val!=values[i] || Gu.fault) {
      GuLog("Registers fail: off=%#x read=%#x expected=%#x fault=%u\n", offsets[i], val, values[i], Gu.fault);
      return FALSE;
    }
  }
  *Ss0=GuRead32(0x82381c);*Ss1=GuRead32(0x823820);
  GuLog("Registers readback: SS0=%#x SS1=%#x fault=%u\n", *Ss0, *Ss1, Gu.fault);
  return !Gu.fault && *Ss0!=0xffffffff && (*Ss0&0xffff0000)!=0xbadf0000 &&
    *Ss0!=0x88888888 && *Ss1<=0xfU;
}
BOOLEAN GuAd104ColdEntry(VOID) {
  UINT32 ss0,ss1;
  return !Gu.started && !Gu.fault && !Gu.retained && Config() &&
    Registers(&ss0,&ss1) && GuEntrySelectorsMatch(ss0,ss1);
}
EFI_STATUS GuAd104Prepare(VOID) {
  UINT32 ss0,ss1,after0,after1;VOID *prom=NULL;struct gpu_unlock_fwsec fw;
  if(Gu.started || Gu.fault || Gu.retained || Gu.allocations || Gu.profile_verified || Gu.prom_verified) {
    GuLog("Prepare fail: state started=%d fault=%d alloc=%d\n", Gu.started, Gu.fault, Gu.allocations);
    return EFI_NOT_READY;
  }
  if(!Config()) return EFI_NOT_READY;
  if(!Registers(&ss0,&ss1)) return EFI_NOT_READY;
  if(!GuAd104AssetsValid()) {
    GuLog("Prepare fail: assets invalid\n");
    return EFI_CRC_ERROR;
  }
  prom=GuAlloc(1048576);if(!prom)return EFI_OUT_OF_RESOURCES;
  GuCopyFromIo(prom,(VOID *)(UINTN)(Gu.bar_base+0x300000),1048576);
  BOOLEAN prom_match = GuAd104PromMatches(prom,1048576);
  if (!prom_match) {
    UINT8 d1[32], d2[32];
    char h1[65], h2[65];
    static const char hex_chars[] = "0123456789abcdef";
    GuSha256(prom, 1048576, d1);
    GuSha256(prom, 626176, d2);
    for (UINTN i = 0; i < 32; i++) {
      h1[i * 2] = hex_chars[d1[i] >> 4];
      h1[i * 2 + 1] = hex_chars[d1[i] & 0x0f];
      h2[i * 2] = hex_chars[d2[i] >> 4];
      h2[i * 2 + 1] = hex_chars[d2[i] & 0x0f];
    }
    h1[64] = '\0'; h2[64] = '\0';
    GuLog("Prepare fail: PROM hash mismatch (1MB=%s vbios=%s)\n", h1, h2);
  }
  int fwsec_err = gpu_unlock_vbios_fwsec(prom,1048576,&fw);
  if (fwsec_err) GuLog("Prepare fail: fwsec parse err=%d\n", fwsec_err);
  BOOLEAN fwsec_match = GuAd104FwsecMatches(&fw);
  if (!fwsec_match) GuLog("Prepare fail: fwsec params mismatch\n");
  BOOLEAN valid=!Gu.fault && prom_match && !fwsec_err && fwsec_match &&
    Config() && Registers(&after0,&after1) && ss0==after0 && ss1==after1;
  GuFree(prom);
  if(!valid)return EFI_COMPROMISED_DATA;
  Gu.prom_verified=TRUE;
  if(!GuLatchEntrySelectors(ss0,ss1)){Gu.prom_verified=FALSE;return EFI_NOT_READY;}
  Gu.profile_verified=TRUE;
  GuLog("Prepare success: cold profile verified and selectors latched (SS0=%#x SS1=%#x)\n", ss0, ss1);
  return EFI_SUCCESS;
}
