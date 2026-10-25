/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "checks.h"

/* Read-only lifecycle predicate shared with the staged adapter. Arming and
 * ownership deliberately are not required here: the last verification runs
 * after the exclusive claim and the USB report have both been closed. */
static inline BOOLEAN GuConfigPostFlrClean(VOID) {
#if GU_STAGED_UNLOCK && GU_ENABLE_UNLOCK && !GU_REHEARSAL
  return Gu.started&&Gu.quiesced&&Gu.saved&&Gu.flrs==1&&Gu.mmio_writes&&
    !Gu.fault&&!Gu.retained&&!Gu.allocations&&!Gu.dma_count&&!Gu.master_attributes_pending&&
    Gu.master_attribute_status==EFI_SUCCESS&&Gu.master_hardware_status==EFI_SUCCESS;
#else
  return FALSE;
#endif
}

/* Compare complete, CPU-owned snapshots without changing either one. REPORT8
 * observed only pre-existing PCIe Device Status CED/URD bits clearing after one
 * successful FLR. They are status, not writable configuration to restore.
 * This permits only that transition, never a whole-register status mask.
 * Current errors/pending must remain zero on EVERY clean post-FLR comparison,
 * including when an error reappears and would match the original snapshot. */
static inline BOOLEAN GuConfigEqual(const UINT32 Expected[64],const UINT32 Actual[64],
  UINT16 PcieCapability,UINT16 *ClearedStatus) {
  if(ClearedStatus)*ClearedStatus=0;
  if(!Expected||!Actual)return FALSE;
  UINTN status_dword=0,shift=0;UINT32 clear_mask=0;UINT16 cleared=0;
  BOOLEAN post_flr=GuConfigPostFlrClean();
#if GU_STAGED_UNLOCK && GU_ENABLE_UNLOCK && !GU_REHEARSAL
  if(post_flr) {
    GU_CAPS before,after;
    if(PcieCapability<0x40||PcieCapability>0xd4||(PcieCapability&3)||
      PcieCapability!=Gu.pcie_cap||
      !GuCheckConfig(Expected,GU_PROFILE_SEGMENT,GU_PROFILE_BUS,GU_PROFILE_DEVICE,GU_PROFILE_FUNCTION,&before)||
      !GuCheckConfig(Actual,GU_PROFILE_SEGMENT,GU_PROFILE_BUS,GU_PROFILE_DEVICE,GU_PROFILE_FUNCTION,&after)||
      before.pcie!=PcieCapability||after.pcie!=PcieCapability||
      before.pcie_version!=Gu.pcie_version||after.pcie_version!=Gu.pcie_version)return FALSE;
    UINTN offset=(UINTN)PcieCapability+0x0a;
    status_dword=offset/4;shift=(offset&3)*8;
    if((Actual[status_dword]>>shift)&0x002fU)return FALSE;
    clear_mask=(UINT32)0x0009U<<shift;
  }
#else
  (void)PcieCapability;
#endif
  for(UINTN i=0;i<64;i++) {
    UINT32 difference=Expected[i]^Actual[i];
    if(!difference)continue;
    if(!post_flr||i!=status_dword||(difference&~(Expected[i]&clear_mask)))return FALSE;
    cleared=(UINT16)(difference>>shift);
  }
  if(ClearedStatus)*ClearedStatus=cleared;
  return TRUE;
}
