/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Exact captured MSI Z790 / RTX 4070 Ti SUPER (10de:2705, AD103). Adapted from
 * windows/ad104-msi/profile.c, which pins a different card/board. A matched
 * profile is not hardware qualification.
 *
 * STATUS: all pins filled in from real captures, including Config()/Registers()
 * below (from probe/, run twice, byte-identical both times, 2026-11-10). This
 * is NOT yet hardware-qualified -- nothing has executed a transaction, written
 * to the GPU, or performed FLR. See the bottom of this file for what remains.
 *
 * Captured so far (2026-11-10):
 *   GPU:     NVIDIA GeForce RTX 4070 Ti SUPER, 10de:2705, subsystem 1462:513e
 *   VBIOS:   95.03.45.80.07.
 *
 * IMPORTANT CORRECTION (2026-11-10, same day): GuAd103PromMatches()'s hash was
 * originally derived from an nvflash --save dump taken from inside a fully
 * booted, driver-loaded Windows session. The probe was then extended to also
 * do a live, read-only hash of the actual BAR0+0x300000 aperture pre-driver,
 * and it DID NOT MATCH the nvflash-derived hash (fail-closed, as designed --
 * nothing proceeded; this is exactly what the verification gate is for).
 * The two 1 MiB images are not the same: nvflash reads VBIOS content back
 * through the already-loaded NVIDIA driver's own path on a running system,
 * which is not the same thing as the raw pre-driver BAR0 aperture content --
 * apparently something elsewhere in the image differs between those two
 * read paths (the FWSEC-relevant region, re-parsed from the live capture
 * below, came back byte-identical either way, which is why fwsec_dump's
 * earlier result was still correct -- but the *whole-image* hash genuinely
 * differs, and that whole-image hash is exactly what GuAd103PromMatches()
 * checks). The hash below is now from the live pre-driver capture, hash
 * confirmed directly against the saved 1 MiB file (not just the printed log
 * line): 1a6631d6317739908aa6fb246858306dfef15901b43237b35b73d424b3880d9e.
 * Lesson for porting to any other board: never derive this particular hash
 * from an OS-level tool's dump. It must come from a live pre-driver EFI read,
 * same mechanism as the real GuAd103Prepare() will use.
 *
 *   Board:   MSI MPG Z790 EDGE WIFI (MS-7D91), BIOS H.E0 2024-09-25
 *   ESP:     disk 1, partition 1, start LBA 2048, 204800 sectors (100 MiB)
 *   Boot mgr: bootmgfw.efi, 3086848 bytes,
 *             sha256 3f8a4cbbc97b89836df92c4895236be1fac4a127665420d90509e897a7298511
 *
 * Config()/Registers() below, captured twice across two separate boot sessions
 * (2026-11-10): found 10de:2705 at segment=0 bus=1 device=0 function=0;
 * GuCheckConfigProbe=1 pm=0x60 pcie=0x78 pcie_version=2 msi=0x68 msix=0x0;
 * BAR0 base 0x81000000 (config dword 4, low 4 type/prefetch bits already 0).
 * Across the two sessions, all 19 register offsets, SS1 (=5), and every
 * config dword matched exactly -- the ONE value that changed was SS0
 * (0x3634054 -> 0x3630050), which is correctly not pinned to a literal in
 * Registers() below (only structurally checked), since it's the dynamic
 * selector value itself, not fixed configuration/status.
 *
 * Of the 19 register offsets, 16 read identically to ad104-msi's own pinned
 * values (same architecture, same SEC2/GSP/FBPA addresses) -- only offset 0
 * (chip/stepping-identifying, expected to differ: 0x193000a1 here vs
 * ad104-msi's 0x194000a1), 0x1183a4 (0x3ff8 vs 0x2ffa) and 0x1fa830
 * (0x3cf7ff0 vs 0x2ef9ff0, plausibly SM-count/die-dependent) differ. All 19
 * are pinned below from this board's own capture regardless of whether they
 * happened to match ad104-msi's, not copied from it.
 *
 * SS1 HISTORY (see SS1_FINDINGS.md for the full writeup and review): this
 * file originally pinned *Ss1==5 from an earlier
 * ownership/register probe capture ("confirmed stable across two
 * sessions") -- but those two sessions were warm reboots/restarts, never a
 * verified full power-off, exactly the gap this file's own earlier caveat
 * had flagged. Corrected to *Ss1==1 after RUN0009.TXT/RUN0010.TXT, two
 * explicitly operator-confirmed genuine full shutdowns (power fully off,
 * >1 minute, Fast Startup disabled), both of which read *Ss1==1. Then
 * RUN0011.TXT -- the *same* confirmed-cold procedure -- read *Ss1==5,
 * falsifying the premise rather than just the guess: SS1's low nibble
 * legitimately varies run to run at cold boot (a third value, 0x2, appears
 * in vendor/unlock_610.c's own comment, matching neither). Reviewed and
 * approved 2026-11-10: SS1 is no longer pinned to any literal. It's handled
 * the same structural way its sibling SS0 already is below -- reject only
 * unambiguously-bad readings, not a specific "correct" value -- while the
 * existing GuLatchEntrySelectors/GuEntrySelectorsMatch machinery (already
 * value-agnostic, comparing against whatever got latched at entry, never a
 * literal) continues to carry the actual consistency guarantee through the
 * rest of the transaction, unchanged.
 *
 * STILL NOT DONE: this file (profile.c/profile.h/port.h) only covers the
 * read-only verification gate. The actual write-capable transaction library
 * -- port.c, checks.c, compat.c, sha256.c, runtime.c, format.c, the vendor/
 * unlock_610.c family, and app.c/boot_io.c/an MSI- or Z790-specific ownership
 * module -- has not been adapted from ad104-msi at all yet. Until that exists
 * and is reviewed the same way this file was, there is no buildable unlock
 * attempt, only a verification gate with nothing behind it to verify.
 */
#include "profile.h"
#include "checks.h"
#include "sha256.h"
#include "compat/compat.h"
#include "boot_io.h" /* GuLog only -- diagnostic visibility, added 2026-11-10
                        after RUN0002/RUN0003 both failed at this file's very
                        first check (Config()/Registers()) with no detail on
                        which field. Every GuLog call below only reports a
                        comparison already being made; none changes what
                        passes or fails. */

/* Same AD102-family firmware/payload the Linux module's 2705 canary used
 * (see docs/ada-2705-report.md): these are architecture-pinned, not
 * board-specific, so they carry over unchanged from ad104-msi/profile.c. */
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
BOOLEAN GuAd103AssetsValid(VOID) {
  for(UINTN i=0;i<5;i++)
    if(!GuAssets[i].name || strcmp(GuAssets[i].name,Pins[i].name) ||
       GuAssets[i].firmware.size!=Pins[i].bytes ||
       !GuHashMatches(GuAssets[i].firmware.data,Pins[i].bytes,Pins[i].hash))return FALSE;
  return TRUE;
}

/* From a live, read-only, pre-driver capture of BAR0+0x300000 by probe/ -- see
 * the IMPORTANT CORRECTION at the top of this file for why this is NOT derived
 * from the earlier nvflash dump. */
BOOLEAN GuAd103PromMatches(const VOID *Data,UINTN Bytes) {
  return Bytes==1048576 && GuHashMatches(Data,Bytes,
    "1a6631d6317739908aa6fb246858306dfef15901b43237b35b73d424b3880d9e");
}

/* Confirmed twice: once against the (wrong-overall-hash) nvflash dump, once
 * against the authentic live pre-driver capture above -- byte-identical
 * results both times, so this geometry was correct even though the PROM
 * hash above was not. dmem_size/pkc_off/interface_off/ucode_id/engine_mask/
 * sig_count/sig_versions/sig_bytes are identical to ad104-msi's own values --
 * expected, since that's shared SEC2 firmware code, not VBIOS-specific. Only
 * the image/data/sig offsets differ, consistent with a different VBIOS image. */
BOOLEAN GuAd103FwsecMatches(const struct gpu_unlock_fwsec *f) {
  return f && f->image_off==0x45b48 && f->image_size==0x10500 &&
    f->imem_pa==0 && f->imem_size==0xf700 && f->imem_va==0 &&
    f->dmem_pa==0 && f->data_off==0xf700 && f->dmem_size==0xd80 &&
    f->pkc_off==0xb24 && f->interface_off==0x1c && f->ucode_id==9 &&
    f->engine_mask==0x400 && f->sig_count==2 && f->sig_versions==3 &&
    f->sig_off==0x45848 && f->sig_bytes==768;
}

static BOOLEAN Config(VOID) {
  UINT32 c[64];UINTN s,b,d,f;GU_CAPS caps;
  if(!Gu.pci || !Gu.pci->GetLocation || !Gu.pci->Pci.Read ||
     Gu.bar_base!=GU_BAR0_BASE || Gu.bar_size!=0x1000000 ||
     EFI_ERROR(Gu.pci->GetLocation(Gu.pci,&s,&b,&d,&f))) {
    GuLog("Config: FAIL early (pci=%#llx bar_base=%#llx bar_size=%#llx GetLocation)\n",
      (unsigned long long)(UINTN)Gu.pci,(unsigned long long)Gu.bar_base,(unsigned long long)Gu.bar_size);
    return FALSE;
  }
  for(UINTN i=0;i<64;i++)
    if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)i*4,&c[i]))) {
      GuLog("Config: FAIL config-space read at dword %u\n",(unsigned)i);
      return FALSE;
    }
  /* This board's own captured dwords (c[6]/c[8] differ from ad104-msi's board;
     c[2]/c[5]/c[7]/c[9] happen to match -- all pinned from our own capture
     regardless). GuCheckConfig itself also requires Bus==1, which this board's
     capture happens to satisfy (segment=0 bus=1 device=0 function=0). */
  BOOLEAN checkConfig=GuCheckConfig(c,s,b,d,f,&caps);
  if(!checkConfig || c[2]!=0x030000a1 || c[4]!=GU_BAR0_BASE || c[5]!=0xc ||
     c[6]!=0x60 || c[7]!=0xc || c[8]!=0x64 || c[9]!=0x4001) {
    GuLog("Config: FAIL GuCheckConfig=%u seg=%u bus=%u dev=%u fun=%u c[2]=%#x(want 0x30000a1) c[4]=%#x(want %#llx) c[5]=%#x(want 0xc) c[6]=%#x(want 0x60) c[7]=%#x(want 0xc) c[8]=%#x(want 0x64) c[9]=%#x(want 0x4001)\n",
      checkConfig,(unsigned)s,(unsigned)b,(unsigned)d,(unsigned)f,c[2],c[4],(unsigned long long)GU_BAR0_BASE,c[5],c[6],c[7],c[8],c[9]);
    return FALSE;
  }
  /* Gu.*_cap was set by app.c's Discover() from this exact same capability
     walk over unchanging config-space bytes, so comparing against it alone
     only checks self-consistency (did anything change since Discover ran),
     not an independent pin -- ad104-msi's own Config() has this same gap.
     Literal values below are this board's own captured capability offsets. */
  BOOLEAN capsOk=caps.pm==Gu.pm_cap && caps.pcie==Gu.pcie_cap &&
    caps.pcie_version==Gu.pcie_version && caps.msi==Gu.msi_cap && caps.msix==Gu.msix_cap &&
    caps.pm==0x60 && caps.pcie==0x78 && caps.pcie_version==2 && caps.msi==0x68 && caps.msix==0;
  if(!capsOk)
    GuLog("Config: FAIL caps pm=%#x(Gu=%#x,want 0x60) pcie=%#x(Gu=%#x,want 0x78) pcie_ver=%u(Gu=%u,want 2) msi=%#x(Gu=%#x,want 0x68) msix=%#x(Gu=%#x,want 0)\n",
      caps.pm,Gu.pm_cap,caps.pcie,Gu.pcie_cap,caps.pcie_version,Gu.pcie_version,caps.msi,Gu.msi_cap,caps.msix,Gu.msix_cap);
  return capsOk;
}
static BOOLEAN Registers(UINT32 *Ss0,UINT32 *Ss1) {
  static const UINT32 offsets[]={0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,
    0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,0x823814,0x823818,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0};
  static const UINT32 values[]={0x193000a1,0x8b8f,0x3ff,0x10,0x1ffffe00,0,
    0x4cb8f,0x4cb8f,0xffffff0f,0xffffff8f,0x233,0x410080,0x3ff8,1,0x45cb8f,0x100000,0x3cf7ff0,1,1};
  for(UINTN i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
    UINT32 v=GuRead32(offsets[i]);
    if(v!=values[i] || Gu.fault) {
      GuLog("Registers: FAIL offset=%#x got=%#x want=%#x fault=%u\n",offsets[i],v,values[i],Gu.fault);
      return FALSE;
    }
  }
  *Ss0=GuRead32(0x82381c);*Ss1=GuRead32(0x823820);
  /* SS1: not pinned to a literal (see header comment and
     SS1_FINDINGS.md) -- structural check matching SS0's own
     pattern. `!(Ss1 & 0x8)` rather than `!=0x8`: GU_SS1_UNLOCK sets bit 3,
     so rejecting the bit being set at all (not just the exact unlocked
     value) also catches a partially-unlocked state (0x9-0xf), and excludes
     none of the three observed-cold values (1, 2, 5 -- all bit-3-clear).
     The inaccessible/out-of-range case is already excluded for free by
     GuLatchEntrySelectors' Ss1<=0xf bound, called right after this
     function returns -- unchanged, not duplicated here. */
  BOOLEAN ok=!Gu.fault && *Ss0!=0xffffffff && (*Ss0&0xffff0000)!=0xbadf0000 &&
    *Ss0!=0x88888888 && !(*Ss1&0x8);
  if(!ok)
    GuLog("Registers: FAIL ss0=%#x ss1=%#x (unlock bit must be clear) fault=%u\n",*Ss0,*Ss1,Gu.fault);
  return ok;
}
BOOLEAN GuAd103ColdEntry(VOID) {
  UINT32 ss0,ss1;
  return !Gu.started && !Gu.fault && !Gu.retained && Config() &&
    Registers(&ss0,&ss1) && GuEntrySelectorsMatch(ss0,ss1);
}
EFI_STATUS GuAd103Prepare(VOID) {
  UINT32 ss0,ss1,after0,after1;VOID *prom=NULL;struct gpu_unlock_fwsec fw;
  if(Gu.started || Gu.fault || Gu.retained || Gu.allocations || Gu.profile_verified ||
     Gu.prom_verified || !Config() || !Registers(&ss0,&ss1))return EFI_NOT_READY;
  if(!GuAd103AssetsValid())return EFI_CRC_ERROR;
  prom=GuAlloc(1048576);if(!prom)return EFI_OUT_OF_RESOURCES;
  GuCopyFromIo(prom,(VOID *)(UINTN)(Gu.bar_base+0x300000),1048576);
  BOOLEAN valid=!Gu.fault && GuAd103PromMatches(prom,1048576) &&
    !gpu_unlock_vbios_fwsec(prom,1048576,&fw) && GuAd103FwsecMatches(&fw) &&
    Config() && Registers(&after0,&after1) && ss0==after0 && ss1==after1;
  GuFree(prom);
  if(!valid)return EFI_COMPROMISED_DATA;
  Gu.prom_verified=TRUE;
  if(!GuLatchEntrySelectors(ss0,ss1)){Gu.prom_verified=FALSE;return EFI_NOT_READY;}
  Gu.profile_verified=TRUE;return EFI_SUCCESS;
}
