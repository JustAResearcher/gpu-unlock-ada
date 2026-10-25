/* SPDX-License-Identifier: GPL-2.0-only */
/* Adapted 2026-10-24 for gpu-unlock-ada; see ../THIRD_PARTY_NOTICES.md. */
#include "app.h"
#include "app_io.h"
#include "compat/compat.h"
#include "checks.h"
#include "config_compare.h"
#include "sha256.h"
#include "probe.h"
#include "ownership.h"
#include "capture.h"
#if GU_REHEARSAL || GU_STAGED_UNLOCK
#include "rehearsal.h"
#include "rehearsal_handoff.h"
#endif
#include "session.h"
#include "unlock_610.h"
#include "gfw_eval.h"
#include <Protocol/GraphicsOutput.h>
#include <IndustryStandard/Acpi.h>
#include <Guid/GlobalVariable.h>
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,
  PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
#if GU_ENABLE_UNLOCK || GU_REHEARSAL
static EFI_GUID GlobalGuid=EFI_GLOBAL_VARIABLE;
#endif
static EFI_SYSTEM_TABLE *Table;
static UINT32 InitialConfig[64];
static const char *Stage;
static BOOLEAN ExpectDetached;
#if !GU_REHEARSAL && !GU_STAGED_UNLOCK
static GU_RELEASE_OWNER PendingOwner;
#endif
static BOOLEAN AppFailed(EFI_STATUS Status){return Status!=EFI_SUCCESS;}
static EFI_STATUS Step(EFI_STATUS Status) {
  GuLog("CHECK stage=%s status=%#llx\n",Stage,(unsigned long long)Status);
  return !GuLogReady() && !AppFailed(Status)?EFI_DEVICE_ERROR:Status;
}
static VOID ConfigLog(const UINT32 *Config) {
  for(UINTN i=0;i<64;i+=4)GuLog("config[%02x]=%08x %08x %08x %08x\n",
    (unsigned)(i*4),Config[i],Config[i+1],Config[i+2],Config[i+3]);
}
static BOOLEAN DisplayCheck(VOID) {
  EFI_HANDLE *handles=NULL;UINTN count=0,physical=0,matched=0;
  EFI_STATUS status=Gu.bs->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&handles);
  if(status==EFI_NOT_FOUND && ExpectDetached)return GuLogReady();
  if(AppFailed(status) || count>128 || (count && !handles)){if(handles)GuFree(handles);return FALSE;}
  for(UINTN i=0;i<count;i++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;EFI_DEVICE_PATH_PROTOCOL *path=NULL;
    status=Gu.bs->HandleProtocol(handles[i],&GopGuid,(VOID **)&g);
    if(AppFailed(status) || !g || !g->Mode){status=EFI_COMPROMISED_DATA;break;}
    if(!g->Mode->FrameBufferBase && !g->Mode->FrameBufferSize)continue;
    physical++;
    status=Gu.bs->HandleProtocol(handles[i],&PathGuid,(VOID **)&path);
    if(AppFailed(status) || !path){status=EFI_UNSUPPORTED;break;}
    EFI_HANDLE parent=NULL;status=Gu.bs->LocateDevicePath(&PciGuid,&path,&parent);
    if(AppFailed(status) || parent!=Gu.handle){status=EFI_UNSUPPORTED;break;}
    if(!g->Mode->Info || g->Mode->SizeOfInfo<sizeof(*g->Mode->Info) ||
       !g->Mode->FrameBufferSize || !g->Mode->Info->VerticalResolution ||
       !g->Mode->Info->HorizontalResolution || g->Mode->Info->PixelFormat>PixelBlueGreenRedReserved8BitPerColor ||
       g->Mode->Info->PixelsPerScanLine<g->Mode->Info->HorizontalResolution ||
       (UINT64)g->Mode->Info->PixelsPerScanLine*g->Mode->Info->VerticalResolution>g->Mode->FrameBufferSize/4) {
      status=EFI_COMPROMISED_DATA;break;
    }
    matched++;
  }
  GuFree(handles);
  GuLog("GOP physical=%zu target=%zu expect_detached=%u status=%#llx\n",(size_t)physical,(size_t)matched,
    ExpectDetached,(unsigned long long)status);
  return !AppFailed(status) && physical==matched && matched==(ExpectDetached?0U:1U) && GuLogReady();
}
#if GU_ENABLE_UNLOCK && !GU_STAGED_UNLOCK
#include "handoff.inc"
#endif
static EFI_STATUS ReadConfig(UINT32 *Config) {
  for(UINTN i=0;i<64;i++)if(AppFailed(GuConfigRead(EfiPciIoWidthUint32,(UINT32)i*4,&Config[i]))) {
    GuLog("PCI config read failed: offset=%#x valid_bytes=%u; snapshot incomplete\n",(unsigned)(i*4),(unsigned)(i*4));
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS ConfigCheck(BOOLEAN Save) {
  UINT32 config[64];GU_CAPS caps;UINTN segment,bus,device,function;
  EFI_STATUS status=Gu.pci->GetLocation(Gu.pci,&segment,&bus,&device,&function);
  GuLog("target location: status=%#llx\n",(unsigned long long)status);
  if(AppFailed(status))return status;
  status=ReadConfig(config);if(AppFailed(status))return status;
  BOOLEAN checked=GuCheckConfig(config,segment,bus,device,function,&caps);
  BOOLEAN valid=checked && (config[2]>>8)==0x030000 &&
    !(config[(caps.pcie+8)/4]&0x00200000U);
  BOOLEAN same=Save || GuConfigEqual(InitialConfig,config,caps.pcie,NULL);
  if(Save || !valid || !same)ConfigLog(config);
  /* Record the already-read evidence without additional PCI queries. The
   * shared comparator permits only the evidenced post-FLR status clearing;
   * control bits, all other configuration, and pre-FLR snapshots stay exact. */
  if(!Save && !same && GuLogReady())for(UINTN i=0;i<64;i++) {
    if(config[i]!=InitialConfig[i])GuLog("APP_CONFIG_DIFFERENCE stage=%s offset=%#x baseline=%#x current=%#x xor=%#x\n",
      Stage,(unsigned)(i*4),InitialConfig[i],config[i],InitialConfig[i]^config[i]);
  }
  GuLog("target BDF=%zu:%zu:%zu.%zu checks=%u pm=%#x pcie=%#x msi=%#x msix=%#x\n",
    (size_t)segment,(size_t)bus,(size_t)device,(size_t)function,checked,
    (unsigned)caps.pm,(unsigned)caps.pcie,(unsigned)caps.msi,(unsigned)caps.msix);
  if(!valid)return EFI_UNSUPPORTED;
  if(!same)return EFI_NOT_READY;
  if(Save){memcpy(InitialConfig,config,sizeof(config));
    GuProfileLogLocation(segment,bus,device,function,config);}
  Gu.pm_cap=caps.pm;Gu.pcie_cap=caps.pcie;Gu.pcie_version=caps.pcie_version;Gu.msi_cap=caps.msi;Gu.msix_cap=caps.msix;
  return EFI_SUCCESS;
}
#if GU_ENABLE_UNLOCK && !GU_STAGED_UNLOCK
static EFI_STATUS RestoreDecode(VOID) {
  UINT32 current[64],normalized[64],after[64];UINTN segment,bus,device,function;
  if(!Gu.pci || !Gu.owned || !Gu.armed || !Gu.display_ok || Gu.started || Gu.allocations ||
    Gu.dma_count || Gu.fault || Gu.retained)return EFI_ACCESS_DENIED;
  EFI_STATUS status=Gu.pci->GetLocation(Gu.pci,&segment,&bus,&device,&function);
  if(AppFailed(status))return status;
  status=ReadConfig(current);if(AppFailed(status))return status;
  GuLogText("config before decode restore (256 valid bytes):\n");ConfigLog(current);
  if(!GuTargetConfig(current,segment,bus,device,function))return EFI_UNSUPPORTED;
  UINT32 missing=InitialConfig[1]&~current[1]&3U;
  memcpy(normalized,current,sizeof(normalized));normalized[1]|=missing;
  /* Restore only cleared, previously enabled IO/MEM bits. Every other bit,
     including BME, status, BAR, ROM and PM/capability state must be unchanged. */
  if(memcmp(normalized,InitialConfig,sizeof(normalized))) {
    GuLogText("decode restore refused: config differs beyond cleared original IO/MEM bits\n");
    return EFI_UNSUPPORTED;
  }
  if(!missing){GuLogText("decode restore not needed\n");return EFI_SUCCESS;}
  if(!Gu.pci->Attributes)return EFI_UNSUPPORTED;
  UINT64 needed=(missing&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(missing&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  UINT64 supported=0,attributes=0;
  status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationSupported,0,&supported);
  GuLog("decode supported attributes: status=%#llx mask=%#llx\n",(unsigned long long)status,(unsigned long long)supported);
  if(AppFailed(status))return status;
  if((supported&needed)!=needed)return EFI_UNSUPPORTED;
  status=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&attributes);
  GuLog("decode current attributes: status=%#llx mask=%#llx\n",(unsigned long long)status,(unsigned long long)attributes);
  if(AppFailed(status))return status;
  GuLog("decode restore intent: saved_command=%#x current_command=%#x missing=%#x attributes=%#llx\n",
    InitialConfig[1]&0xffff,current[1]&0xffff,missing,(unsigned long long)needed);
  if(!GuLogReady() || Gu.fault)return EFI_DEVICE_ERROR;
  /* Never use EFI_PCI_DEVICE_ENABLE: it would also enable bus mastering. */
  EFI_STATUS enabled=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationEnable,needed,NULL);
  GuLog("decode enable status=%#llx\n",(unsigned long long)enabled);
  if(!GuLogReady() || Gu.fault)return AppFailed(enabled)?enabled:EFI_DEVICE_ERROR;
  /* Even an error may follow a partial hardware effect. Capture it if readable,
     but never turn an API error into success merely because readback matches. */
  status=ReadConfig(after);
  BOOLEAN matched=FALSE;
  if(!AppFailed(status)) {
    GuLogText("config after decode restore (256 valid bytes):\n");ConfigLog(after);
    matched=!memcmp(after,InitialConfig,sizeof(after));GuLog("decode readback matched=%u\n",matched);
  }
  if(AppFailed(enabled))return enabled;
  if(AppFailed(status))return status;
  if(!GuLogReady() || Gu.fault)return EFI_DEVICE_ERROR;
  return matched?EFI_SUCCESS:EFI_COMPROMISED_DATA;
}
#endif
static EFI_STATUS FindTarget(VOID) {
  EFI_HANDLE *handles=NULL;UINTN count=0,found=0;
  EFI_STATUS status=Gu.bs->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,&count,&handles);
  GuLog("PCI enumeration: status=%#llx handles=%zu\n",(unsigned long long)status,(size_t)count);
  if(AppFailed(status) || !handles || !count || count>256){if(handles)GuFree(handles);return EFI_NOT_FOUND;}
  for(UINTN i=0;i<count;i++) {
    EFI_PCI_IO_PROTOCOL *pci=NULL;UINT32 id=0;
    status=Gu.bs->HandleProtocol(handles[i],&PciGuid,(VOID **)&pci);
    if(!AppFailed(status) && pci)status=pci->Pci.Read(pci,EfiPciIoWidthUint32,0,1,&id);
    if(AppFailed(status) || !pci){GuLog("PCI scan failed: index=%zu status=%#llx\n",(size_t)i,(unsigned long long)status);status=EFI_DEVICE_ERROR;break;}
    if(id==GU_BOARD_ID){found++;Gu.handle=handles[i];Gu.pci=pci;}
  }
  GuFree(handles);GuLog("PCI target matches=%zu status=%#llx\n",(size_t)found,(unsigned long long)status);
  if(AppFailed(status) || found!=1)return EFI_UNSUPPORTED;
  return ConfigCheck(TRUE);
}
static EFI_STATUS BarCheck(VOID) {
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *r=NULL;
  EFI_STATUS status=Gu.pci->GetBarAttributes(Gu.pci,0,NULL,(VOID **)&r);
  GuLog("bar0 query: status=%#llx\n",(unsigned long long)status);
  if(!AppFailed(status)) {
    if(r && r->Desc==ACPI_ADDRESS_SPACE_DESCRIPTOR && r->Len==sizeof(*r)-3)
      GuLog("bar0 descriptor: type=%u granularity=%llu min=%#llx max=%#llx length=%#llx translation=%#llx\n",
        (unsigned)r->ResType,(unsigned long long)r->AddrSpaceGranularity,(unsigned long long)r->AddrRangeMin,
        (unsigned long long)r->AddrRangeMax,(unsigned long long)r->AddrLen,(unsigned long long)r->AddrTranslationOffset);
    if(!r || r->Desc!=ACPI_ADDRESS_SPACE_DESCRIPTOR || r->Len!=sizeof(*r)-3 || r->ResType!=0 ||
      !r->AddrRangeMin || (r->AddrRangeMin&0xffffffULL) || r->AddrLen!=0x1000000 || r->AddrTranslationOffset ||
      /* UEFI describes an inclusive end; pinned EDK2 PciBusDxe instead returns
         PciBar.Alignment. Both representations describe this exact 16 MiB BAR. */
      (r->AddrRangeMax!=r->AddrRangeMin+r->AddrLen-1 && r->AddrRangeMax!=r->AddrLen-1) ||
      (r->AddrSpaceGranularity!=32 && r->AddrSpaceGranularity!=64) || ((UINT8 *)(r+1))[0]!=ACPI_END_TAG_DESCRIPTOR)status=EFI_UNSUPPORTED;
    else {
      UINT8 *raw=(UINT8 *)r,sum=0;
      for(UINTN i=0;i<sizeof(*r)+2;i++)sum=(UINT8)(sum+raw[i]);
      if(raw[sizeof(*r)+1] && sum)status=EFI_COMPROMISED_DATA;
      else {
        Gu.bar_base=r->AddrRangeMin;Gu.bar_size=r->AddrLen;
        UINT64 config_base=InitialConfig[4]&~15ULL;
        if((InitialConfig[4]&6)==4)config_base|=(UINT64)InitialConfig[5]<<32;
        if((InitialConfig[4]&1) || config_base!=Gu.bar_base)status=EFI_COMPROMISED_DATA;
#if GU_ENABLE_UNLOCK || GU_REHEARSAL
        if(Gu.bar_base!=GU_PROFILE_BAR_BASE || Gu.bar_size!=GU_PROFILE_BAR_SIZE)status=EFI_UNSUPPORTED;
#endif
        GuLog("PROFILE bar_base=%llx bar_size=%llx\n",(unsigned long long)Gu.bar_base,(unsigned long long)Gu.bar_size);
      }
    }
  }
  if(r)GuFree(r);return status;
}
static EFI_STATUS ColdCheck(VOID) {
  UINT32 chip=GuRead32(0),vram=GuRead32(0x1183a4);
  GuLog("PROFILE chip=%08x vram_mib=%u\n",chip,vram);
  if((chip&0xfff00000U)!=0x19300000U || vram<16000 || vram>16384)return EFI_UNSUPPORTED;
#if GU_ENABLE_UNLOCK || GU_REHEARSAL
  if(chip!=GU_BOARD_CHIP || vram!=GU_PROFILE_VRAM)return EFI_UNSUPPORTED;
#endif
  struct gpu_unlock_gfw_sample s={.config_ok=true,.plm=GuRead32(GU_GFW_PLM_OFFSET),
    .boot=GuRead32(GU_GFW_BOOT_OFFSET),.cpuctl=GuRead32(GU_GFW_CPUCTL_OFFSET)};
  UINT32 hi=GuRead32(0x1fa828),plm=GuRead32(GU_FEAT_PLM_OFFSET),
    ss0=GuRead32(GU_FEAT_SS0_OFFSET),ss1=GuRead32(GU_FEAT_SS1_OFFSET);
  GuLog("cold state: chip=%#x FEAT=%#x GFW=%#x/%#x/%#x WPR2_HI=%#x SS0=%#x SS1=%#x\n",
    chip,plm,s.plm,s.boot,s.cpuctl,hi,ss0,ss1);
  if(Gu.fault || gpu_unlock_gfw_assess(&s) || hi>>4 || plm!=0xffffff8f ||
    (ss0&0x88888888U) || ss1!=7)return EFI_NOT_READY;
  return EFI_SUCCESS;
}
static EFI_STATUS PromCheck(VOID) {return GuProbeProm(GU_ENABLE_UNLOCK || GU_REHEARSAL);}
#if GU_ENABLE_UNLOCK && !GU_STAGED_UNLOCK
static EFI_STATUS Arm(VOID) {
#if GU_SESSION_HELPER
  if(!GuLogReady())return EFI_DEVICE_ERROR;
  EFI_STATUS status=GuIoConsumeSession();
  if(!AppFailed(status))GuLogText("SESSION_CONSUMED: one-time permit flushed and read back; all hardware gates remain required.\n");
  return !GuLogReady() && !AppFailed(status)?EFI_DEVICE_ERROR:status;
#else
  if(!Table->ConIn || !Table->ConIn->Reset || !Table->ConIn->ReadKeyStroke)return EFI_UNSUPPORTED;
  EFI_STATUS status=Table->ConIn->Reset(Table->ConIn,FALSE);if(AppFailed(status))return status;
  GuLogText("READY: RTX 4070 Ti SUPER / EXPERIMENTAL WINDOWS PORT / Windows preloaded.\n");
  if(PendingOwner.agent)GuLogText("U will release the RTX firmware driver, restore verified access settings if needed, then attempt WPR_CFG -> FBPA -> WPR -> FEAT in ONE SEC2 launch, then SS0/SS1 if required gates pass; log mismatches and recover.\n");
  GuLogText("Press U once. One SEC2 fire containing four ordered PLM writes, then selectors/FLR/Windows on full success. The last frame may remain visible. Power OFF on STOP. Esc/30s cancels.\n");
  if(!GuLogReady())return EFI_DEVICE_ERROR;
  for(UINTN i=0;i<300;i++) {
    EFI_INPUT_KEY key;status=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);
    if(!AppFailed(status)) {
      if(key.ScanCode==0x17 || key.UnicodeChar==27)return EFI_ABORTED;
      if(key.UnicodeChar=='u' || key.UnicodeChar=='U')return EFI_SUCCESS;
    } else if(status!=EFI_NOT_READY)return status;
    GuStall(100000);
  }
  return EFI_TIMEOUT;
#endif
}
#endif
static VOID PausePreflightFailure(VOID) {
  if(GuScreenDetached || Gu.started || !Table->ConIn || !Table->ConIn->ReadKeyStroke)return;
  if(Table->ConIn->Reset)Table->ConIn->Reset(Table->ConIn,FALSE);
  GuConsole("Preflight stopped before unlock. See the FINAL stage above and REPORT.TXT.\n");
  GuConsole("Press Enter or Esc to return to firmware; automatic return in 60 seconds. U cannot start an attempt here.\n");
  for(UINTN i=0;i<600;i++) {
    EFI_INPUT_KEY key;EFI_STATUS s=Table->ConIn->ReadKeyStroke(Table->ConIn,&key);
    if(!AppFailed(s) && (key.ScanCode==0x17 || key.UnicodeChar==27 || key.UnicodeChar=='\r'))break;
    if(AppFailed(s) && s!=EFI_NOT_READY)break;
    Gu.bs->Stall(100000);
  }
}
#if GU_ENABLE_UNLOCK && !GU_STAGED_UNLOCK
static VOID Halt(VOID) {
  Gu.retained=TRUE;
  GuLogText("STOP: cleanup cannot be proven. Resources remain reserved.\nPOWER OFF using the physical power button; do not continue to Windows.\n");
  /* No ResetSystem, ExitBootServices, retry, protocol close or memory release. */
  for(;;)Gu.bs->Stall(1000000);
}
static VOID LogContainment(VOID) {
  /* Called only after any TPL_NOTIFY containment section has ended. */
  GuLog("BME containment: attribute_status=%#llx hardware_status=%#llx command=%#x cached=%#llx pending=%u\n",
    (unsigned long long)Gu.master_attribute_status,(unsigned long long)Gu.master_hardware_status,
    Gu.master_last_command,(unsigned long long)Gu.master_last_attributes,Gu.master_attributes_pending);
}
static VOID ReleaseHalt(EFI_STATUS Status,BOOLEAN Attempted) {
  /* A failed claim may follow interface removal/replacement during logging.
     Resolve, validate and clear BME without a callback window or stale access. */
  EFI_TPL old=Gu.bs->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS refreshed=GuRefreshTargetPci();
  EFI_STATUS contained=AppFailed(refreshed)?refreshed:GuClearMaster();
  Gu.bs->RestoreTPL(old);
  LogContainment();
  GuLog("FINAL status=%#llx stage=%s attempted=%u driver_release=1 clean=0 containment=%#llx pci_available=%u\n",
    (unsigned long long)Status,Stage,Attempted,(unsigned long long)contained,(unsigned)(Gu.pci!=NULL));
  Halt();
}
#endif
#if !GU_ENABLE_UNLOCK && !GU_REHEARSAL
static EFI_STATUS DiagnosticMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *St) {
  if(!St || !St->BootServices)return EFI_INVALID_PARAMETER;
  Table=St;GuInit(St->BootServices,NULL);Gu.image=Image;GuScreenDetached=FALSE;ExpectDetached=FALSE;
  Stage="usb-log";EFI_STATUS status=GuIoOpen(Image,St);
  if(AppFailed(status))goto done;
  GuLogText("DIAGNOSTIC_ONLY: GPU writes, firmware execution, reset and driver detach are disabled.\n");
  Stage="target";status=Step(FindTarget());if(AppFailed(status))goto done;
  Stage="bar0";status=Step(BarCheck());if(AppFailed(status))goto done;
  Stage="cold-state";status=Step(ColdCheck());if(AppFailed(status))goto done;
  Stage="prom";status=Step(PromCheck());if(AppFailed(status))goto done;
  GuLogText("Collecting firmware metadata into TOPOLOGY.TXT. The screen may pause while the file is written.\n");
  Stage="firmware-capture";status=Step(GuCaptureFirmware(St));if(AppFailed(status))goto done;
  Stage="display";status=Step(DisplayCheck()?EFI_SUCCESS:EFI_UNSUPPORTED);if(AppFailed(status))goto done;
  Stage="owner-observe";status=Step(GuFindReleaseOwner(St,&PendingOwner,TRUE));if(AppFailed(status))goto done;
  Stage="complete";
  GuLogText("DIAGNOSTIC_FINISHED eligible=0 gpu_writes=0; metadata requires independent review\n");
done:
  if(!AppFailed(status) && !GuLogReady())status=EFI_DEVICE_ERROR;
  if(GuLogReady())GuLog("FINAL mode=diagnostic status=%#llx stage=%s gpu_writes=%u\n",
    (unsigned long long)status,Stage,Gu.mmio_writes);
  if(!AppFailed(status) && !GuLogReady())status=EFI_DEVICE_ERROR;
  if(!GuLogReady())GuConsole("USB logging failed; REPORT.TXT may be missing or incomplete.\n");
  EFI_STATUS closed=GuIoClose();
  if(!AppFailed(status))status=closed;
  GuConsole("Diagnostic finished. No unlock was attempted. Return REPORT.TXT and TOPOLOGY.TXT for review.\n");
  if(AppFailed(closed))GuConsole("USB report close failed; the report may be incomplete.\n");
  if(AppFailed(status))PausePreflightFailure();
  return status;
}
#endif
#if GU_REHEARSAL
#include "rehearsal_main.inc"
#endif
#if GU_STAGED_UNLOCK
#include "staged_main.inc"
#endif
#if GU_ENABLE_UNLOCK && !GU_STAGED_UNLOCK
static EFI_STATUS __attribute__((unused)) UnlockMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *St);
#endif
EFI_STATUS EFIAPI EfiMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *St) {
#if GU_STAGED_UNLOCK
  return StagedMain(Image,St);
#elif GU_REHEARSAL
  return RehearsalMain(Image,St);
#elif !GU_ENABLE_UNLOCK
  return DiagnosticMain(Image,St);
#else
  return UnlockMain(Image,St);
#endif
}
#if GU_ENABLE_UNLOCK && !GU_STAGED_UNLOCK
static EFI_STATUS UnlockMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *St) {
  if(!St || !St->BootServices || !St->RuntimeServices)return EFI_INVALID_PARAMETER;
  WindowsImage=NULL;WindowsVolume=NULL;WindowsDevicePath=NULL;GuScreenDetached=FALSE;ExpectDetached=FALSE;
  Table=St;GuInit(St->BootServices,NULL);Gu.image=Image;Gu.display_check=DisplayCheck;
  memset(&PendingOwner,0,sizeof(PendingOwner));
  Stage="usb-log";EFI_STATUS status=GuIoOpen(Image,St);BOOLEAN attempted=FALSE,release_attempted=FALSE;
  if(AppFailed(status))goto finish;
  UINT8 secure=0xff;UINTN bytes=1;
  Stage="secure-boot";
  status=St->RuntimeServices->GetVariable(L"SecureBoot",&GlobalGuid,NULL,&bytes,&secure);
  if(AppFailed(status) || bytes!=1 || secure){status=EFI_SECURITY_VIOLATION;goto finish;}
  status=Step(status);if(AppFailed(status))goto finish;
  Stage="watchdog";status=Step(Gu.bs->SetWatchdogTimer(0,0,0,NULL));if(AppFailed(status))goto finish;
  Stage="target";status=Step(FindTarget());if(AppFailed(status))goto finish;
  Stage="display";Gu.display_ok=DisplayCheck();status=Step(Gu.display_ok?EFI_SUCCESS:EFI_UNSUPPORTED);if(AppFailed(status))goto finish;
  Stage="ownership";status=GuClaimController();
  if(status==EFI_ACCESS_DENIED) {
    status=GuFindReleaseOwner(St,&PendingOwner,TRUE);
    if(!AppFailed(status))GuLogText("Exclusive ownership deferred until arming and explicit driver release.\n");
  }
  if(!AppFailed(status) && !PendingOwner.agent)status=EFI_UNSUPPORTED;
  status=Step(status);if(AppFailed(status))goto finish;
  Stage="config-recheck";status=Step(ConfigCheck(FALSE));if(AppFailed(status))goto finish;
  Stage="bar0";status=Step(BarCheck());if(AppFailed(status))goto finish;
  Stage="cold-state";status=Step(ColdCheck());if(AppFailed(status))goto finish;
  Stage="prom";status=Step(PromCheck());if(AppFailed(status))goto finish;
  Stage="assets";status=Step(GuLoadAssets());if(AppFailed(status))goto finish;
  Stage="windows-preload";status=Step(PreloadWindows());if(AppFailed(status))goto finish;
  Stage="arming";status=Step(Arm());if(AppFailed(status))goto finish;
  Stage="armed-config";status=Step(ConfigCheck(FALSE));if(AppFailed(status))goto finish;
  Stage="armed-display";Gu.display_ok=DisplayCheck();status=Step(Gu.display_ok?EFI_SUCCESS:EFI_UNSUPPORTED);if(AppFailed(status))goto finish;
  Stage="armed-cold-state";status=Step(ColdCheck());if(AppFailed(status))goto finish;
  if(PendingOwner.agent) {
    GU_RELEASE_OWNER current;
    Stage="owner-recheck";
    GuLogText("Arming confirmed: release RTX firmware driver once, then revalidate.\n");
    if(!GuLogReady()){status=EFI_DEVICE_ERROR;goto finish;}
    /* Durable intent precedes the last owner snapshot. No file IO between a
       successful final comparison and DisconnectController. */
    status=GuFindReleaseOwner(St,&current,FALSE);
    if(!AppFailed(status) && !GuSameReleaseOwner(&PendingOwner,&current))status=EFI_ABORTED;
    if(AppFailed(status)){status=Step(status);goto finish;}
    EFI_PCI_IO_PROTOCOL *prior=Gu.pci;
    Stage="driver-release";release_attempted=TRUE;Gu.armed=TRUE;Gu.pci=NULL;
    GuScreenDetached=TRUE;ExpectDetached=TRUE;
    EFI_STATUS released=Gu.bs->DisconnectController(Gu.handle,current.agent,NULL);
    EFI_TPL refresh_tpl=Gu.bs->RaiseTPL(TPL_NOTIFY);
    EFI_STATUS refreshed=GuRefreshTargetPci();
    Gu.bs->RestoreTPL(refresh_tpl);
    GuLog("driver release: status=%#llx refresh=%#llx same_pci=%u\n",
      (unsigned long long)released,(unsigned long long)refreshed,(unsigned)(Gu.pci==prior));
    status=Step(AppFailed(released)?released:AppFailed(refreshed)?refreshed:Gu.pci!=prior?EFI_COMPROMISED_DATA:EFI_SUCCESS);
    if(AppFailed(status))goto finish;
    Stage="ownership-after-release";status=Step(GuClaimController());if(AppFailed(status))goto finish;
    Stage="display-after-release";Gu.display_ok=DisplayCheck();status=Step(Gu.display_ok?EFI_SUCCESS:EFI_UNSUPPORTED);if(AppFailed(status))goto finish;
    Stage="decode-after-release";status=Step(RestoreDecode());if(AppFailed(status))goto finish;
    Stage="config-after-release";status=Step(ConfigCheck(FALSE));if(AppFailed(status))goto finish;
    Stage="bar0-after-release";status=Step(BarCheck());if(AppFailed(status))goto finish;
    Stage="cold-after-release";status=Step(ColdCheck());if(AppFailed(status))goto finish;
    Stage="prom-after-release";status=Step(PromCheck());if(AppFailed(status))goto finish;
  }
  GuLogText("ARMED: one transaction is starting. Do not reset or remove the USB.\n");
  if(!GuLogReady()){status=EFI_DEVICE_ERROR;goto finish;}
  Stage="transaction";Gu.armed=TRUE;attempted=TRUE;
  UINT64 mask=0xffffffffULL;
  struct pci_dev device={.vendor=0x10de,.device=0x2705,.dev={.dma_mask=&mask,.coherent_dma_mask=0xffffffffULL},.pm_cap=Gu.pm_cap,.refs=1};
  struct gpu_unlock_610_shared shared={0};
  int result=gpu_unlock_610_load_shared(&device,&shared);
  if(!result)result=gpu_unlock_610_unlock(&device,&shared);
  /* Also covers failures before GuStarted, including BME-enable readback. */
  EFI_STATUS contained=GuClearMaster();
  LogContainment();
  GuLog("transaction result=%d containment=%#llx started=%u quiesced=%u fault=%u retained=%u maps=%u FLR=%u\n",
    result,(unsigned long long)contained,Gu.started,Gu.quiesced,Gu.fault,Gu.retained,Gu.dma_count,Gu.flrs);
  if(AppFailed(contained) || !GuSafeToReturn() || Gu.allocations || (Gu.started && result))Halt();
  gpu_unlock_610_release_shared(&shared);Gu.armed=FALSE;
  status=result?EFI_DEVICE_ERROR:EFI_SUCCESS;
finish:
  if(!AppFailed(status) && attempted && release_attempted) {
    Stage="post-unlock-handoff";status=RestoreDisplayForWindows(PendingOwner.agent);
  }
  if(release_attempted && (AppFailed(status) || Gu.fault))ReleaseHalt(status,attempted);
  if(attempted && !GuSafeToReturn())Halt();
  if(!AppFailed(status) && attempted)
    GuLogText("UNLOCK_VERIFIED: selectors survived FLR and GOP reconnect; Windows handoff pending.\n");
  if(GuLogReady())GuLog("FINAL status=%#llx stage=%s attempted=%u driver_release=%u clean=%u\n",(unsigned long long)status,Stage,attempted,release_attempted,GuSafeToReturn());
  else GuConsole("Preflight or logging failed; no new attempt will start.\n");
  if(!GuLogReady()){status=EFI_DEVICE_ERROR;if(release_attempted)ReleaseHalt(status,attempted);if(Gu.started)Halt();}
  if(attempted && !GuSafeToReturn())Halt();
  EFI_STATUS closed=GuIoClose();
  if(AppFailed(closed)){if(release_attempted)ReleaseHalt(closed,attempted);if(Gu.started)Halt();status=closed;}
  if(Gu.owned) {
    closed=Gu.bs->CloseProtocol(Gu.handle,&PciGuid,Image,NULL);
    if(AppFailed(closed)){if(release_attempted)ReleaseHalt(closed,attempted);if(Gu.started)Halt();status=closed;}else Gu.owned=FALSE;
  }
  GuReleaseAssets();
  if(!AppFailed(status) && attempted) {
    GuConsole("UNLOCK_SUCCESS: full-rate selectors verified after reset and display restoration. Starting Windows.\n");
    UINTN bytes=0;CHAR16 *data=NULL;
    status=Gu.bs->StartImage(WindowsImage,&bytes,&data);
    if(data)GuFree(data);
    /* Returning after a hardware attempt could permit accidental rerun. */
    GuScreenDetached=TRUE;Halt();
  }
  else GuConsole("Attempt cancelled or rejected. Returning without another attempt.\n");
  if(WindowsImage)Gu.bs->UnloadImage(WindowsImage);
  GuFree(WindowsDevicePath);WindowsDevicePath=NULL;
  if(AppFailed(status) && status!=EFI_ABORTED && status!=EFI_TIMEOUT && !release_attempted)PausePreflightFailure();
  return status;
}
#endif
