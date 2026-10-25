/* SPDX-License-Identifier: GPL-2.0-only */
/* An explicitly armed transaction between the independently rehearsed stages.
 * Never compile this into the diagnostic or firmware-only rehearsal. */
#if !GU_STAGED_UNLOCK || !GU_ENABLE_UNLOCK || GU_REHEARSAL || GU_SESSION_HELPER
#error Invalid staged unlock entry-point configuration
#endif
#include "staged_state.h"

static EFI_STATUS StagedStep(EFI_STATUS Status) {
  Status=Step(Status);
  if(Status!=EFI_SUCCESS)return EFI_ERROR(Status)?Status:EFI_DEVICE_ERROR;
  return Gu.fault || Gu.retained || !GuLogReady()?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS StagedArm(VOID) {
  if(!Table->ConIn || !Table->ConIn->Reset || !Table->ConIn->ReadKeyStroke)return EFI_UNSUPPORTED;
  EFI_STATUS s=Table->ConIn->Reset(Table->ConIn,FALSE);
  if(s!=EFI_SUCCESS)return s;
  GuLogText("READY: RTX 4070 Ti SUPER AD103 / STAGED UNLOCK TEST 0.6.4.\n");
  GuLogText("U releases the boot-display drivers, runs one GPU firmware transaction with DMA and one function reset, restores the display, and starts Windows only after verification.\n");
  GuLogText("This test accepts the observed PCIe status-flag clearing after reset; all other PCI configuration must still match. Display restoration and Windows boot after the unlock remain unverified.\n");
  GuLogText("The display can black-screen or hang. No VBIOS flash, Windows driver installation, or permanent boot change is performed by this app.\n");
  GuLogText("Press U once to attempt the unlock. Esc or 120 seconds cancels. R does not arm this test. If Windows does not appear after 2 minutes, power OFF and remove USB.\n");
  if(!GuLogReady() || Gu.fault)return EFI_DEVICE_ERROR;
  for(UINTN i=0;i<1200;i++) {
    EFI_INPUT_KEY key;s=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);
    if(s==EFI_SUCCESS) {
      if(key.ScanCode==0x17 || key.UnicodeChar==27)return EFI_ABORTED;
      if(key.UnicodeChar=='u' || key.UnicodeChar=='U')return EFI_SUCCESS;
    } else if(s!=EFI_NOT_READY)return s;
    s=Gu.bs->Stall(100000);if(s!=EFI_SUCCESS)return s;
  }
  return EFI_TIMEOUT;
}
static VOID StagedContainmentLog(EFI_STATUS Status) {
  GuLog("BME containment: status=%#llx attribute_status=%#llx hardware_status=%#llx command=%#x cached=%#llx pending=%u\n",
    (unsigned long long)Status,(unsigned long long)Gu.master_attribute_status,
    (unsigned long long)Gu.master_hardware_status,Gu.master_last_command,
    (unsigned long long)Gu.master_last_attributes,Gu.master_attributes_pending);
}
static VOID __attribute__((noreturn)) StagedHalt(EFI_STATUS Status,BOOLEAN Attempted,BOOLEAN LogOpen) {
  /* No stale console or PCI pointer is trusted after a callback failure. The
   * only attempted hardware change here is containment of bus mastering on a
   * freshly identified target. No retry, reset, reconnect, free, or handoff. */
  GuScreenDetached=TRUE;
  EFI_STATUS contained=EFI_NOT_READY;
  if(GuRehearsalMutated() && Gu.bs->RaiseTPL && Gu.bs->RestoreTPL) {
    EFI_TPL previous=Gu.bs->RaiseTPL(TPL_NOTIFY);
    EFI_STATUS refreshed=GuStagedRefreshPci();
    contained=refreshed==EFI_SUCCESS?GuClearMaster():refreshed;
    Gu.bs->RestoreTPL(previous);
  }
  if(LogOpen && GuLogReady()) {
    StagedContainmentLog(contained);
    GuLog("FINAL mode=staged-unlock-test status=%#llx stage=%s mutated=%u unlock_attempted=%u app_mmio_writes=%u flrs=%u dma=%u started=%u quiesced=%u retained=%u windows_handoff=blocked\n",
      (unsigned long long)Status,Stage,GuRehearsalMutated(),Attempted,Gu.mmio_writes,
      Gu.flrs,Gu.dma_count,Gu.started,Gu.quiesced,Gu.retained);
  }
  /* Each report write was already flushed. Closing is safe only when the port
   * can prove no uncertain transaction lifetime; otherwise retain everything. */
  if(LogOpen && GuSafeToReturn())GuIoClose();
  Gu.retained=TRUE;
  for(;;)Gu.bs->Stall(1000000);
}
static EFI_STATUS StagedMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *St) {
  if(!St || !St->BootServices || !St->RuntimeServices)return EFI_INVALID_PARAMETER;
  Table=St;GuInit(St->BootServices,NULL);Gu.image=Image;
  GuScreenDetached=FALSE;ExpectDetached=FALSE;
  BOOLEAN attempted=FALSE;
  Stage="usb-log";EFI_STATUS status=GuIoOpen(Image,St);
  if(status!=EFI_SUCCESS)goto before_release;
  GuLogText("STAGED_UNLOCK_TEST: GPU register writes, DMA and one FLR are enabled only after U and all board/ROM/firmware/asset checks.\n");
  UINT8 secure=0xff;UINTN bytes=1;
  Stage="secure-boot";
  status=St->RuntimeServices->GetVariable?St->RuntimeServices->GetVariable(L"SecureBoot",&GlobalGuid,NULL,&bytes,&secure):EFI_UNSUPPORTED;
  GuLog("SecureBoot query: status=%#llx bytes=%zu value=%u\n",(unsigned long long)status,(size_t)bytes,(unsigned)secure);
  if(status!=EFI_SUCCESS || bytes!=1 || secure){status=EFI_SECURITY_VIOLATION;goto before_release;}
  status=StagedStep(status);if(status!=EFI_SUCCESS)goto before_release;
  Stage="target";status=StagedStep(FindTarget());if(status!=EFI_SUCCESS)goto before_release;
  Stage="bar0";status=StagedStep(BarCheck());if(status!=EFI_SUCCESS)goto before_release;
  Stage="cold-state";status=StagedStep(ColdCheck());if(status!=EFI_SUCCESS)goto before_release;
  Stage="prom";status=StagedStep(PromCheck());if(status!=EFI_SUCCESS)goto before_release;
  Stage="display";status=StagedStep(DisplayCheck()?EFI_SUCCESS:EFI_UNSUPPORTED);if(status!=EFI_SUCCESS)goto before_release;
  /* Asset allocation and Windows loading precede the complete handle baseline. */
  Stage="assets";status=StagedStep(GuLoadAssets());if(status!=EFI_SUCCESS)goto before_release;
  Stage="windows-preload";status=StagedStep(GuRehearsalWindowsPrepare(St));if(status!=EFI_SUCCESS)goto before_release;
  Stage="staged-preflight";status=StagedStep(GuRehearsalPrepare(St));if(status!=EFI_SUCCESS)goto before_release;
  Stage="arming";status=StagedStep(StagedArm());if(status!=EFI_SUCCESS)goto before_release;
  Stage="watchdog";status=StagedStep(Gu.bs->SetWatchdogTimer?Gu.bs->SetWatchdogTimer(0,0,0,NULL):EFI_UNSUPPORTED);
  if(status!=EFI_SUCCESS)goto before_release;
  Stage="armed-config";status=StagedStep(ConfigCheck(FALSE));if(status!=EFI_SUCCESS)goto before_release;
  Stage="armed-cold";status=StagedStep(ColdCheck());if(status!=EFI_SUCCESS)goto before_release;
  GuLogText("STAGED_UNLOCK_INTENT: U received; one release / unlock / restoration transaction.\n");
  if(!GuLogReady() || Gu.fault){status=EFI_DEVICE_ERROR;goto before_release;}
  Gu.armed=TRUE;GuScreenDetached=TRUE;Stage="driver-release";
  status=GuStagedRelease();
  if(status!=EFI_SUCCESS || Gu.fault || !GuLogReady()) {
    if(status==EFI_SUCCESS)status=EFI_DEVICE_ERROR;
    if(GuRehearsalMutated())StagedHalt(status,FALSE,TRUE);
    Gu.armed=FALSE;GuScreenDetached=FALSE;goto before_release;
  }
  Stage="exclusive-claim";status=StagedStep(GuStagedClaim());if(status!=EFI_SUCCESS)StagedHalt(status,FALSE,TRUE);
  Gu.display_check=GuStagedDetached;
  Stage="claimed-config";status=StagedStep(ConfigCheck(FALSE));if(status!=EFI_SUCCESS)StagedHalt(status,FALSE,TRUE);
  Stage="claimed-bar0";status=StagedStep(BarCheck());if(status!=EFI_SUCCESS)StagedHalt(status,FALSE,TRUE);
  Stage="claimed-cold";status=StagedStep(ColdCheck());if(status!=EFI_SUCCESS)StagedHalt(status,FALSE,TRUE);
  Stage="claimed-prom";status=StagedStep(PromCheck());if(status!=EFI_SUCCESS)StagedHalt(status,FALSE,TRUE);
  GuLogText("ARMED: verified exclusive ownership; starting the single GPU transaction.\n");
  if(!GuLogReady() || Gu.fault)StagedHalt(EFI_DEVICE_ERROR,FALSE,TRUE);
  Stage="transaction";attempted=TRUE;
  UINT64 mask=0xffffffffULL;
  struct pci_dev device={.vendor=0x10de,.device=0x2705,.dev={.dma_mask=&mask,.coherent_dma_mask=0xffffffffULL},.pm_cap=Gu.pm_cap,.refs=1};
  struct gpu_unlock_610_shared shared={0};
  int result=gpu_unlock_610_load_shared(&device,&shared);
  if(!result)result=gpu_unlock_610_unlock(&device,&shared);
  EFI_STATUS contained=GuClearMaster();
  StagedContainmentLog(contained);
  GuLog("transaction result=%d started=%u quiesced=%u fault=%u retained=%u maps=%u FLR=%u app_mmio_writes=%u\n",
    result,Gu.started,Gu.quiesced,Gu.fault,Gu.retained,Gu.dma_count,Gu.flrs,Gu.mmio_writes);
  if(result || contained!=EFI_SUCCESS || !GuSafeToReturn() || Gu.allocations || Gu.dma_count ||
     !Gu.started || !Gu.quiesced || !Gu.saved || Gu.flrs!=1 || !Gu.mmio_writes || Gu.fault || !GuLogReady())
    StagedHalt(EFI_DEVICE_ERROR,TRUE,TRUE);
  gpu_unlock_610_release_shared(&shared);
  Stage="transaction-post-state";status=StagedStep(GuStagedPostState(TRUE));if(status!=EFI_SUCCESS)StagedHalt(status,TRUE,TRUE);
  Stage="exclusive-release";status=StagedStep(GuStagedComplete());if(status!=EFI_SUCCESS)StagedHalt(status,TRUE,TRUE);
  Stage="display-restoration";status=GuStagedRestore();
  if(status!=EFI_SUCCESS || Gu.fault || !GuLogReady())StagedHalt(status==EFI_SUCCESS?EFI_DEVICE_ERROR:status,TRUE,TRUE);
  Stage="restoration-check";status=GuRehearsalRestored();if(status!=EFI_SUCCESS)StagedHalt(status,TRUE,TRUE);
  GuScreenDetached=FALSE;
  Stage="restored-config";status=StagedStep(ConfigCheck(FALSE));if(status!=EFI_SUCCESS)StagedHalt(status,TRUE,TRUE);
  Stage="restored-bar0";status=StagedStep(BarCheck());if(status!=EFI_SUCCESS)StagedHalt(status,TRUE,TRUE);
  Stage="restored-unlock";status=StagedStep(GuStagedPostState(TRUE));if(status!=EFI_SUCCESS)StagedHalt(status,TRUE,TRUE);
  GuReleaseAssets();
  if(Gu.fault || Gu.retained || Gu.owned || !GuSafeToReturn())StagedHalt(EFI_DEVICE_ERROR,TRUE,TRUE);
  Gu.armed=FALSE;
  Stage="windows-handoff";
  GuLogText("UNLOCK_VERIFIED: selectors survived FLR and the rehearsed display restoration; Windows handoff pending.\n");
  GuLog("FINAL mode=staged-unlock-test status=0 stage=windows-handoff mutated=1 unlock_attempted=1 app_mmio_writes=%u flrs=%u dma=%u windows_handoff=pending\n",Gu.mmio_writes,Gu.flrs,Gu.dma_count);
  if(!GuLogReady() || Gu.fault)StagedHalt(EFI_DEVICE_ERROR,TRUE,TRUE);
  status=GuIoClose();if(status!=EFI_SUCCESS)StagedHalt(status,TRUE,FALSE);
  /* The handoff revalidates ownership and unlock state silently after close. */
  status=GuRehearsalWindowsStart();
  StagedHalt(status==EFI_SUCCESS?EFI_ABORTED:status,TRUE,FALSE);
before_release:
  if(GuRehearsalMutated())StagedHalt(status,attempted,TRUE);
  Gu.armed=FALSE;GuScreenDetached=FALSE;
  EFI_STATUS cancelled=GuRehearsalWindowsCancel();
  if(cancelled!=EFI_SUCCESS){Stage="preflight-cleanup";StagedHalt(cancelled,FALSE,TRUE);}
  GuReleaseAssets();
  if(!GuSafeToReturn() || Gu.retained){Stage="asset-cleanup";StagedHalt(EFI_DEVICE_ERROR,FALSE,TRUE);}
  if(GuLogReady())GuLog("FINAL mode=staged-unlock-test status=%#llx stage=%s mutated=0 unlock_attempted=0 app_mmio_writes=%u\n",
    (unsigned long long)status,Stage,Gu.mmio_writes);
  if(!GuLogReady() && status==EFI_SUCCESS)status=EFI_DEVICE_ERROR;
  EFI_STATUS closed=GuIoClose();if(closed!=EFI_SUCCESS)status=closed;
  GuConsole("Unlock test finished before driver release. No unlock was attempted. Return REPORT.TXT.\n");
  if(status!=EFI_SUCCESS && status!=EFI_ABORTED && status!=EFI_TIMEOUT)PausePreflightFailure();
  return status;
}
