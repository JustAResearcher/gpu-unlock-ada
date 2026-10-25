/* SPDX-License-Identifier: GPL-2.0-only */
/* Included only in the separate firmware rehearsal. No unlock transaction. */
#if !GU_REHEARSAL || GU_ENABLE_UNLOCK
#error Invalid rehearsal entry-point configuration
#endif
static EFI_STATUS RehearsalArm(VOID) {
  if(!Table->ConIn || !Table->ConIn->Reset || !Table->ConIn->ReadKeyStroke)return EFI_UNSUPPORTED;
  EFI_STATUS s=Table->ConIn->Reset(Table->ConIn,FALSE);
  if(s!=EFI_SUCCESS)return s;
  GuLogText("READY: firmware release/restoration rehearsal; unlock payload is absent.\n");
  GuLogText("Press R to release three boot-display drivers, reconnect them, then start Windows only after verification.\n");
  GuLogText("This active firmware test may blank or hang the display. If Windows has not appeared after 2 minutes, power OFF, remove USB and restore security settings.\n");
  GuLogText("Esc or 120 seconds cancels before release. No automatic retry.\n");
  if(!GuLogReady() || Gu.fault)return EFI_DEVICE_ERROR;
  for(UINTN i=0;i<1200;i++) {
    EFI_INPUT_KEY key;s=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);
    if(s==EFI_SUCCESS) {
      if(key.ScanCode==0x17 || key.UnicodeChar==27)return EFI_ABORTED;
      if(key.UnicodeChar=='r' || key.UnicodeChar=='R')return EFI_SUCCESS;
    } else if(s!=EFI_NOT_READY)return s;
    s=Gu.bs->Stall(100000);if(s!=EFI_SUCCESS)return s;
  }
  return EFI_TIMEOUT;
}
static VOID __attribute__((noreturn)) RehearsalHalt(EFI_STATUS Status,BOOLEAN LogOpen) {
  /* A driver callback can partially mutate hardware even when it fails. Do not
   * retry, reconnect blindly, reset, return to the boot manager or start Windows.
   * Only the already-open USB report is flushed/closed before retaining state. */
  if(LogOpen && GuLogReady())GuLog("FINAL mode=firmware-rehearsal status=%#llx stage=%s mutated=%u unlock_attempted=0\n",
    (unsigned long long)Status,Stage,GuRehearsalMutated());
  if(LogOpen)GuIoClose();
  GuConsole("STOP: firmware restoration or Windows handoff was not verified. Power OFF, remove USB, and restore security settings.\n");
  Gu.retained=TRUE;
  for(;;)Gu.bs->Stall(1000000);
}
static EFI_STATUS RehearsalMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *St) {
  if(!St || !St->BootServices || !St->RuntimeServices)return EFI_INVALID_PARAMETER;
  Table=St;GuInit(St->BootServices,NULL);Gu.image=Image;
  GuScreenDetached=FALSE;ExpectDetached=FALSE;
  Stage="usb-log";EFI_STATUS status=GuIoOpen(Image,St);
  if(status!=EFI_SUCCESS)goto before_release;
  GuLogText("REHEARSAL_ONLY: active firmware driver callbacks; no unlock payload, direct GPU writes, DMA, FLR, flashing or persistent boot changes.\n");
  UINT8 secure=0xff;UINTN bytes=1;
  Stage="secure-boot";
  status=St->RuntimeServices->GetVariable?St->RuntimeServices->GetVariable(L"SecureBoot",&GlobalGuid,NULL,&bytes,&secure):EFI_UNSUPPORTED;
  GuLog("SecureBoot query: status=%#llx bytes=%zu value=%u\n",(unsigned long long)status,(size_t)bytes,(unsigned)secure);
  if(status!=EFI_SUCCESS || bytes!=1 || secure){status=EFI_SECURITY_VIOLATION;goto before_release;}
  status=Step(status);if(status!=EFI_SUCCESS)goto before_release;
  Stage="target";status=Step(FindTarget());if(status!=EFI_SUCCESS)goto before_release;
  Stage="bar0";status=Step(BarCheck());if(status!=EFI_SUCCESS)goto before_release;
  Stage="cold-state";status=Step(ColdCheck());if(status!=EFI_SUCCESS)goto before_release;
  Stage="prom";status=Step(PromCheck());if(status!=EFI_SUCCESS)goto before_release;
  Stage="display";status=Step(DisplayCheck()?EFI_SUCCESS:EFI_UNSUPPORTED);if(status!=EFI_SUCCESS)goto before_release;
  /* Loading Windows before the graph baseline avoids an unaccounted-for image
   * appearing between ownership validation and driver release. */
  Stage="windows-preload";status=Step(GuRehearsalWindowsPrepare(St));if(status!=EFI_SUCCESS)goto before_release;
  Stage="rehearsal-preflight";status=Step(GuRehearsalPrepare(St));if(status!=EFI_SUCCESS)goto before_release;
  Stage="arming";status=Step(RehearsalArm());if(status!=EFI_SUCCESS)goto before_release;
  Stage="watchdog";status=Step(Gu.bs->SetWatchdogTimer?Gu.bs->SetWatchdogTimer(0,0,0,NULL):EFI_UNSUPPORTED);
  if(status!=EFI_SUCCESS)goto before_release;
  Stage="armed-config";status=Step(ConfigCheck(FALSE));if(status!=EFI_SUCCESS)goto before_release;
  Stage="armed-cold";status=Step(ColdCheck());if(status!=EFI_SUCCESS)goto before_release;
  GuLogText("REHEARSAL_INTENT: R received; beginning staged release/restoration.\n");
  if(!GuLogReady() || Gu.fault){status=EFI_DEVICE_ERROR;goto before_release;}
  Gu.armed=TRUE;GuScreenDetached=TRUE;Stage="release-and-restore";
  status=GuRehearsalRun();
  if(status!=EFI_SUCCESS || Gu.fault || !GuLogReady()) {
    if(status==EFI_SUCCESS)status=EFI_DEVICE_ERROR;
    if(GuRehearsalMutated())RehearsalHalt(status,TRUE);
    Gu.armed=FALSE;GuScreenDetached=FALSE;goto before_release;
  }
  Stage="restoration-check";status=GuRehearsalRestored();
  if(status!=EFI_SUCCESS)RehearsalHalt(status,TRUE);
  GuScreenDetached=FALSE;
  Stage="restored-config";status=Step(ConfigCheck(FALSE));if(status!=EFI_SUCCESS)RehearsalHalt(status,TRUE);
  Stage="restored-bar0";status=Step(BarCheck());if(status!=EFI_SUCCESS)RehearsalHalt(status,TRUE);
  Stage="restored-cold-state";status=Step(ColdCheck());if(status!=EFI_SUCCESS)RehearsalHalt(status,TRUE);
  if(Gu.mmio_writes || Gu.flrs || Gu.dma_count || Gu.allocations || Gu.started || Gu.owned)
    RehearsalHalt(EFI_COMPROMISED_DATA,TRUE);
  Stage="windows-handoff";
  GuLogText("REHEARSAL_RESTORED: display ownership and PCI/cold state verified.\n");
  GuLogText("FINAL mode=firmware-rehearsal status=0 stage=windows-handoff mutated=1 unlock_attempted=0 app_mmio_writes=0 windows_handoff=pending\n");
  if(!GuLogReady() || Gu.fault)RehearsalHalt(EFI_DEVICE_ERROR,TRUE);
  status=GuIoClose();if(status!=EFI_SUCCESS)RehearsalHalt(status,FALSE);
  /* Start performs the final silent restoration check after USB close. */
  status=GuRehearsalWindowsStart();
  GuScreenDetached=TRUE;
  RehearsalHalt(status==EFI_SUCCESS?EFI_ABORTED:status,FALSE);
before_release:
  if(GuRehearsalMutated())RehearsalHalt(status,TRUE);
  Gu.armed=FALSE;GuScreenDetached=FALSE;
  EFI_STATUS cancelled=GuRehearsalWindowsCancel();
  if(cancelled!=EFI_SUCCESS){Stage="preflight-cleanup";RehearsalHalt(cancelled,TRUE);}
  if(GuLogReady())GuLog("FINAL mode=firmware-rehearsal status=%#llx stage=%s mutated=0 unlock_attempted=0 app_mmio_writes=%u\n",
    (unsigned long long)status,Stage,Gu.mmio_writes);
  if(!GuLogReady() && status==EFI_SUCCESS)status=EFI_DEVICE_ERROR;
  EFI_STATUS closed=GuIoClose();if(closed!=EFI_SUCCESS)status=closed;
  GuConsole("Rehearsal finished before driver release. No unlock was attempted. Return REPORT.TXT.\n");
  if(status!=EFI_SUCCESS)PausePreflightFailure();
  return status;
}
