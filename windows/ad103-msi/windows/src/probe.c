/* SPDX-License-Identifier: GPL-2.0-only */
#include "probe.h"
#include "app_io.h"
#include "runtime.h"
#include "sha256.h"
#include "vbios_610.h"

static VOID HexHash(const VOID *Data, UINTN Bytes, char Hex[65]) {
  static const char digits[]="0123456789abcdef";
  UINT8 digest[32]; GuSha256(Data,Bytes,digest);
  for(UINTN i=0;i<32;i++){Hex[2*i]=digits[digest[i]>>4];Hex[2*i+1]=digits[digest[i]&15];}
  Hex[64]=0;
}
VOID GuProfileLogLocation(UINTN Segment,UINTN Bus,UINTN Device,UINTN Function,
                          const UINT32 Config[64]) {
  GuLog("PROFILE version=1 id=%08x subsystem=%08x segment=%u bus=%u device=%u function=%u\n",
    Config[0],Config[11],(unsigned)Segment,(unsigned)Bus,(unsigned)Device,(unsigned)Function);
}
EFI_STATUS GuProbeProm(BOOLEAN Verify) {
  char prom_hash[65],rom_hash[65];
  if(!Gu.pci || !Gu.pci->RomImage || Gu.pci->RomSize<512 || Gu.pci->RomSize>2U*1024U*1024U)
    return EFI_UNSUPPORTED;
  VOID *prom=GuAlloc(GU_PROM_VIEW_BYTES); if(!prom)return EFI_OUT_OF_RESOURCES;
  GuCopyFromIo(prom,(VOID *)(UINTN)(Gu.bar_base+GU_PROM_BAR_OFFSET),GU_PROM_VIEW_BYTES);
  if(Gu.fault){GuFree(prom);return EFI_DEVICE_ERROR;}
  HexHash(prom,GU_PROM_VIEW_BYTES,prom_hash);
  HexHash(Gu.pci->RomImage,(UINTN)Gu.pci->RomSize,rom_hash);
  struct gpu_unlock_fwsec fw;
  int parsed=gpu_unlock_vbios_fwsec(prom,GU_PROM_VIEW_BYTES,&fw);
  GuLog("PROFILE prom_size=%u prom_sha256=%s\n",GU_PROM_VIEW_BYTES,prom_hash);
  GuLog("PROFILE rom_size=%u rom_sha256=%s\n",(unsigned)Gu.pci->RomSize,rom_hash);
  GuLog("PROFILE fwsec_parsed=%u\n",parsed==0);
  /* Dump hashes only. Never export the user's ROM or NVIDIA firmware. */
  BOOLEAN match=!Verify || (Gu.pci->RomSize==GU_PROFILE_ROM_SIZE &&
    !strcmp(prom_hash,GU_PROFILE_PROM_HASH) && !strcmp(rom_hash,GU_PROFILE_ROM_HASH));
  GuFree(prom);
  if(parsed)return EFI_UNSUPPORTED;
  if(!match)return EFI_CRC_ERROR;
  return GuLogReady()?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
