/* SPDX-License-Identifier: GPL-2.0-only */
/* Production staged entry, PCI/ROM checks, logger and post-state observer.
 * Mock only the separately tested adapter, transaction and asset boundaries.
 * No GPU, disk, Windows loader, firmware binary or UEFI variable is accessed. */
#define main diagnostic_fixture_main
#include "test_diagnostic.c"
#undef main
#include "rehearsal.h"
#include "rehearsal_handoff.h"
#include "staged_state.h"
#include "unlock_610.h"
#include <setjmp.h>

static jmp_buf stopped;
static unsigned prepared,planned,released,claimed,completed,restored,checked;
static unsigned cancelled,started,asset_loads,asset_releases,core_loads,core_runs,core_releases;
static unsigned arming_waits,post_samples;
static BOOLEAN mutated,transaction_done;

/* Adapter callback pinning is covered by test_staged_adapter. This explicit
 * boundary retains the production identity/status checks for entry tests. */
EFI_STATUS GuStagedRefreshPci(VOID) {
  assert(mutated && GuScreenDetached);return GuRefreshTargetPci();
}

static EFI_STATUS boundary(const char *name) {
  char value[80];snprintf(value,sizeof(value),"%s-error",name);
  if(is(value))return EFI_DEVICE_ERROR;
  snprintf(value,sizeof(value),"%s-warning",name);
  return is(value)?EFI_WARN_UNKNOWN_GLYPH:EFI_SUCCESS;
}
EFI_STATUS __wrap_GuLoadAssets(VOID) {
  assert(GuLogReady() && !prepared && !Gu.armed);asset_loads++;
  return boundary("assets");
}
VOID __wrap_GuReleaseAssets(VOID) {
  assert(!mutated || (restored && checked));asset_releases++;
  if(is("asset-release-fault"))Gu.fault=TRUE;
  if(is("asset-release-retained"))Gu.retained=TRUE;
}
EFI_STATUS GuRehearsalWindowsPrepare(EFI_SYSTEM_TABLE *table) {
  assert(table==&st && asset_loads==1 && !planned && GuLogReady());prepared++;
  return boundary("preload");
}
EFI_STATUS GuRehearsalPrepare(EFI_SYSTEM_TABLE *table) {
  assert(table==&st && prepared==1 && GuLogReady() && !Gu.armed);planned++;
  if(is("cancel-error") || is("cancel-warning"))return EFI_UNSUPPORTED;
  return boundary("prepare");
}
EFI_STATUS GuStagedRelease(VOID) {
  assert(planned==1 && GuScreenDetached && Gu.armed && GuLogReady());released++;
  if(is("release-before-mutation"))return EFI_NOT_READY;
  mutated=TRUE;
  if(is("release-fault"))Gu.fault=TRUE;
  return boundary("release");
}
EFI_STATUS GuStagedClaim(VOID) {
  assert(mutated && released==1 && GuScreenDetached && Gu.armed);claimed++;
  Gu.owned=TRUE;Gu.display_ok=TRUE;
  if(is("claim-cleanup-failed")){Gu.retained=TRUE;Gu.fault=TRUE;return EFI_DEVICE_ERROR;}
  if(is("claimed-config"))config[4]^=0x1000000;
  if(is("status-preclaim-clear"))config[0x58/4]&=0xffff;
  if(is("claimed-rom"))prom[4000]^=1;
  return boundary("claim");
}
BOOLEAN GuStagedDetached(VOID) {return mutated && claimed==1 && !completed;}
EFI_STATUS GuStagedComplete(VOID) {
  assert(claimed==1 && core_runs==1 && core_releases==1 && Gu.owned && !Gu.fault && post_samples==1);
  assert(GuSafeToReturn() && !Gu.dma_count && !Gu.allocations);completed++;
  EFI_STATUS status=boundary("complete");
  if(status==EFI_SUCCESS)Gu.owned=FALSE;
  return status;
}
EFI_STATUS GuStagedRestore(VOID) {
  assert(completed==1 && !Gu.owned && GuScreenDetached && GuLogReady());restored++;
  if(is("restore-fault"))Gu.fault=TRUE;
  if(is("restored-config"))config[4]^=0x1000000;
  if(is("status-reassert-restored"))config[0x58/4]|=0x90000;
  return boundary("restore");
}
EFI_STATUS GuRehearsalRestored(VOID) {assert(restored==1);checked++;return boundary("restoration-check");}
BOOLEAN GuRehearsalMutated(VOID) {return mutated;}
EFI_STATUS GuRehearsalWindowsCancel(VOID) {assert(!mutated);cancelled++;return boundary("cancel");}
EFI_STATUS GuRehearsalWindowsStart(VOID) {
  assert(mutated && !GuScreenDetached && !GuLogReady() && restored && checked && file_closes==4);
  assert(core_releases==1 && asset_releases==1 && post_samples==2 && !Gu.owned && !Gu.armed);
  assert(GuStagedPostState(FALSE)==EFI_SUCCESS);started++;return boundary("start");
}
int gpu_unlock_610_load_shared(struct pci_dev *device,struct gpu_unlock_610_shared *shared) {
  assert(device->vendor==0x10de && device->device==0x2705 && shared && Gu.owned && Gu.armed);
  assert(asset_loads==1 && claimed==1 && !core_runs);core_loads++;
  return is("core-load-error")?-EIO:0;
}
int gpu_unlock_610_unlock(struct pci_dev *device,const struct gpu_unlock_610_shared *shared) {
  (void)device;assert(shared && core_loads==1 && Gu.owned && Gu.armed && GuStagedDetached());core_runs++;
  if(is("core-before-start-error"))return -EIO;
  transaction_done=TRUE;Gu.started=TRUE;Gu.quiesced=TRUE;Gu.saved=TRUE;Gu.flrs=1;Gu.mmio_writes=17;
  memcpy(Gu.saved_config,config,sizeof(config));
  if(!strncmp(scenario,"status-",7)){
    config[0x58/4]&=0xffff;
    if(is("status-partial-correctable"))config[0x58/4]|=0x10000;
    if(is("status-partial-unsupported"))config[0x58/4]|=0x80000;
    if(is("status-new-nonfatal"))config[0x58/4]|=0x20000;
    if(is("status-new-fatal"))config[0x58/4]|=0x40000;
    if(is("status-new-pending"))config[0x58/4]|=0x200000;
    if(is("status-control"))config[0x58/4]^=1;
    if(is("status-other-dword"))config[4]^=0x1000000;
    if(is("status-stale-cap"))Gu.pcie_cap=0x60;
    if(is("status-stale-version"))Gu.pcie_version=1;
  }
  if(is("core-error"))return -EIO;
  if(is("core-positive-result"))return 1;
  if(is("core-fault"))Gu.fault=TRUE;
  if(is("core-retained"))Gu.retained=TRUE;
  if(is("core-not-started"))Gu.started=FALSE;
  if(is("core-not-quiesced"))Gu.quiesced=FALSE;
  if(is("core-not-saved"))Gu.saved=FALSE;
  if(is("core-no-flr"))Gu.flrs=0;
  if(is("core-no-writes"))Gu.mmio_writes=0;
  if(is("core-dma-left"))Gu.dma_count=1;
  return 0;
}
void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared) {
  assert(shared && core_runs==1 && GuSafeToReturn() && Gu.owned);core_releases++;
}
static EFI_STATUS EFIAPI staged_config(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
 UINT32 offset,UINTN count,VOID *out) {
  (void)self;assert(count==1 && width<=EfiPciIoWidthUint32 && offset+(1U<<width)<=256);
  pci_reads++;memcpy(out,(UINT8*)config+offset,1U<<width);
  if(is("refresh-config-warning") || (is("post-config-warning") && core_releases))return EFI_WARN_UNKNOWN_GLYPH;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI staged_mmio(EFI_PCI_IO_PROTOCOL *self,EFI_PCI_IO_PROTOCOL_WIDTH width,
 UINT8 bar,UINT64 offset,UINTN count,VOID *out) {
  if(!transaction_done)return mem_read(self,width,bar,offset,count,out);
  assert(width==EfiPciIoWidthUint32 && !bar && count==1);gpu_reads++;
  UINT32 value=0;
  switch(offset) {
  case 0:value=GU_BOARD_CHIP;break;
  case 0x1183a4:value=GU_PROFILE_VRAM;break;
  case 0x1fa7cc:case 0x9a0148:case 0x1fa7c4:case GU_FEAT_PLM_OFFSET:value=0xffffffff;break;
  case GU_FEAT_SS0_OFFSET:
    post_samples++;value=(is("post-selectors") || (is("restored-selectors") && restored))?0:0x88888888;break;
  case GU_FEAT_SS1_OFFSET:value=8;break;
  case GU_GFW_PLM_OFFSET:value=1;break;
  case GU_GFW_BOOT_OFFSET:value=0xff;break;
  case GU_GFW_CPUCTL_OFFSET:value=0x10;break;
  case GU_FEAT_READOUT_1_OFFSET:value=is("post-readout")?1:0;break;
  case 0x1fa824:case 0x1fa828:break;
  default:assert(!"Unexpected post-state MMIO access");
  }
  memcpy(out,&value,4);
  return is("post-mmio-warning")?EFI_WARN_UNKNOWN_GLYPH:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI staged_protocol(EFI_HANDLE handle,EFI_GUID *guid,VOID **out) {
  EFI_STATUS status=protocol(handle,guid,out);
  return is("refresh-protocol-warning") && eq(guid,&pci_guid)?EFI_WARN_UNKNOWN_GLYPH:status;
}
static EFI_STATUS EFIAPI staged_location(EFI_PCI_IO_PROTOCOL *self,UINTN *s,UINTN *b,UINTN *d,UINTN *f) {
  EFI_STATUS status=location(self,s,b,d,f);
  return is("refresh-location-warning")?EFI_WARN_UNKNOWN_GLYPH:status;
}
static EFI_STATUS EFIAPI secure(CHAR16 *name,EFI_GUID *guid,UINT32 *attrs,UINTN *bytes,VOID *data) {
  (void)name;(void)guid;(void)attrs;assert(*bytes==1);*(UINT8*)data=is("secure-on");return boundary("secure");
}
static EFI_STATUS EFIAPI watchdog(UINTN timeout,UINT64 code,UINTN size,CHAR16 *data) {
  assert(!timeout && !code && !size && !data && !mutated);return boundary("watchdog");
}
static EFI_STATUS EFIAPI staged_input_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self,BOOLEAN extended) {
  (void)self;(void)extended;return boundary("input-reset");
}
static EFI_STATUS EFIAPI staged_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self,EFI_INPUT_KEY *key_out) {
  (void)self;key_out->ScanCode=0;key_out->UnicodeChar='U';
  if(!GuLogReady())key_out->UnicodeChar='\r';
  if(is("lowercase-u"))key_out->UnicodeChar='u';
  if(is("status-prearm-clear")&&GuLogReady())config[0x58/4]&=0xffff;
  if(is("escape"))key_out->UnicodeChar=27;
  if(is("r-key") || is("stall-warning"))key_out->UnicodeChar='R';
  if(is("timeout"))return EFI_NOT_READY;
  return boundary("key");
}
static EFI_STATUS EFIAPI staged_stall(UINTN usec) {
  if(Gu.retained && usec==1000000)longjmp(stopped,1);
  if(usec==100000 && GuLogReady())arming_waits++;
  return boundary("stall");
}
static EFI_STATUS EFIAPI staged_flush(EFI_FILE_PROTOCOL *file) {
  if(is("intent-log-fail") && strstr(report,"STAGED_UNLOCK_INTENT:"))return EFI_DEVICE_ERROR;
  if(is("handoff-log-fail") && strstr(report,"windows_handoff=pending"))return EFI_DEVICE_ERROR;
  return flush(file);
}
int main(int argc,char **argv) {
  assert(argc==2);scenario=argv[1];setup();
  if(!strncmp(scenario,"status-",7)){
    config[0x58/4]=0x00092930;
    if(is("status-clear-correctable"))config[0x58/4]=0x00012930;
    if(is("status-clear-unsupported"))config[0x58/4]=0x00082930;
  }
  rt.GetVariable=secure;bs.SetWatchdogTimer=watchdog;bs.Stall=staged_stall;
  bs.HandleProtocol=staged_protocol;pci.GetLocation=staged_location;
  pci.Pci.Read=staged_config;pci.Mem.Read=staged_mmio;
  con_in.ReadKeyStroke=staged_key;con_in.Reset=staged_input_reset;
  for(unsigned i=0;i<4;i++)files[i].p.Flush=staged_flush;
  if(!strncmp(scenario,"refresh-",8)) {
    GuInit(&bs,&pci);Gu.handle=gpu_handle;
    assert(GuRefreshTargetPci()==EFI_DEVICE_ERROR && !Gu.pci && !mutation_calls);
    printf("staged entry %s: refresh warning rejected\n",scenario);return 0;
  }
  if(is("wrong-subsystem"))config[11]^=1;
  if(is("wrong-rom"))prom[4000]^=1;
  EFI_STATUS status=EFI_SUCCESS;int halted=setjmp(stopped);
  if(!halted)status=EfiMain(app_handle,&st);
  assert(!mutation_calls && !Gu.allocations);
  if(mutated)assert(halted && !cancelled && Gu.retained);
  else if(is("cancel-error") || is("cancel-warning") || is("asset-release-retained"))
    assert(halted && cancelled==1 && !started);
  else assert(!halted && status!=EFI_SUCCESS && cancelled==1 && !started);
  if(is("start-return") || is("start-error") || is("start-warning") || is("lowercase-u") ||
     !strncmp(scenario,"status-clear-",13))
    assert(started==1 && core_runs==1 && completed==1 && restored==1 && post_samples==3);
  else assert(!started);
  if(is("escape") || is("timeout") || is("r-key") || is("key-warning") || is("input-reset-warning") ||
     is("stall-warning") || is("watchdog-warning") || is("intent-log-fail"))
    assert(!released && !claimed && !core_runs);
  if(is("timeout") || is("r-key"))assert(arming_waits==1200);
  if(is("claim-error") || is("claim-warning") || is("claim-cleanup-failed") || is("claimed-config") || is("claimed-rom"))
    assert(!core_loads && !completed && !restored);
  if(is("claimed-config"))assert(strstr(report,"APP_CONFIG_DIFFERENCE stage=claimed-config offset=0x10") &&
    strstr(report,"baseline=0x90000000 current=0x91000000 xor=0x1000000"));
  if(is("restored-config"))assert(strstr(report,"APP_CONFIG_DIFFERENCE stage=restored-config offset=0x10") &&
    strstr(report,"baseline=0x90000000 current=0x91000000 xor=0x1000000"));
  if(!strncmp(scenario,"core-",5))assert(!core_releases && !completed && !restored);
  if(is("post-selectors") || is("post-readout") || is("post-mmio-warning") || is("post-config-warning"))
    assert(core_releases==1 && !completed && !restored);
  if(is("release-error") || is("release-warning") || is("release-fault"))assert(!claimed);
  if(is("complete-error") || is("complete-warning"))assert(!restored);
  if(is("secure-on") || is("secure-warning") || is("wrong-subsystem") || is("wrong-rom"))assert(!asset_loads);
  if(is("status-prearm-clear"))assert(!released&&!core_runs&&!restored);
  if(is("status-preclaim-clear"))assert(claimed==1&&!core_runs&&!restored);
  if(!strncmp(scenario,"status-clear-",13)){
    assert(!strstr(report,"APP_CONFIG_DIFFERENCE")&&config[0x58/4]==0x2930);
  }
  if(!strncmp(scenario,"status-",7)&&strncmp(scenario,"status-clear-",13)&&
     !is("status-new-pending")&&!is("status-prearm-clear")&&!is("status-preclaim-clear")){
    assert(restored==1&&!started);
    assert(strstr(report,"stage=restored-config"));
  }
  if(!mutated)assert(!Gu.mmio_writes && !Gu.flrs && !Gu.started && !Gu.owned);
  if(mutated && !restored)assert(!asset_releases);
  if(!Gu.retained)assert(pool_allocs==pool_frees);
  printf("staged entry %s: release=%u claim=%u core=%u complete=%u restore=%u start=%u halted=%u direct_mutations=0\n",
    scenario,released,claimed,core_runs,completed,restored,started,halted);
  return 0;
}
