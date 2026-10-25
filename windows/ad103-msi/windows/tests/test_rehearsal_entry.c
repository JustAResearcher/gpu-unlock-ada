/* SPDX-License-Identifier: GPL-2.0-only */
/* Reuse the synthetic PCI/ROM/USB fixture, testing the production app/logger
 * around mocked adapter/handoff boundaries. Their real cores have own tests. */
#define main diagnostic_fixture_main
#include "test_diagnostic.c"
#undef main
#include "rehearsal.h"
#include "rehearsal_handoff.h"
#include <setjmp.h>
static jmp_buf stopped;
static unsigned prepared,planned,ran,restored,cancelled,started,arming_waits;
static BOOLEAN mutated;
EFI_STATUS GuRehearsalPrepare(EFI_SYSTEM_TABLE *t){
  assert(t==&st && prepared==1 && GuLogReady() && !Gu.armed);planned++;
  return (is("prepare-fail")||is("cancel-error"))?EFI_UNSUPPORTED:EFI_SUCCESS;
}
EFI_STATUS GuRehearsalRun(VOID){
  assert(planned==1 && GuScreenDetached && Gu.armed && GuLogReady());ran++;
  if(is("run-before-mutation"))return EFI_NOT_READY;
  mutated=TRUE;
  if(is("run-partial"))return EFI_DEVICE_ERROR;
  if(is("run-warning"))return EFI_WARN_UNKNOWN_GLYPH;
  if(is("run-fault"))Gu.fault=TRUE;
  return EFI_SUCCESS;
}
EFI_STATUS GuRehearsalRestored(VOID){restored++;return is("restore-fail")?EFI_NOT_READY:EFI_SUCCESS;}
BOOLEAN GuRehearsalMutated(VOID){return mutated;}
EFI_STATUS GuRehearsalWindowsPrepare(EFI_SYSTEM_TABLE *t){
  assert(t==&st && !planned && GuLogReady());prepared++;
  return is("preload-fail")?EFI_NOT_FOUND:EFI_SUCCESS;
}
EFI_STATUS GuRehearsalWindowsCancel(VOID){assert(!mutated);cancelled++;return is("cancel-error")?EFI_DEVICE_ERROR:EFI_SUCCESS;}
EFI_STATUS GuRehearsalWindowsStart(VOID){
  assert(mutated && !GuScreenDetached && !GuLogReady() && restored && file_closes==4);
  started++;return is("start-error")?EFI_LOAD_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI secure(CHAR16 *name,EFI_GUID *g,UINT32 *attrs,UINTN *bytes,VOID *data){
  (void)name;(void)g;(void)attrs;assert(*bytes==1);*(UINT8*)data=is("secure-on");
  return is("secure-warning")?EFI_WARN_UNKNOWN_GLYPH:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI watchdog(UINTN timeout,UINT64 code,UINTN size,CHAR16 *data){
  assert(!timeout && !code && !size && !data && !mutated);
  return is("watchdog-warning")?EFI_WARN_UNKNOWN_GLYPH:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI rehearsal_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self,EFI_INPUT_KEY *k){
  (void)self;k->ScanCode=0;k->UnicodeChar='R';
  if(is("escape"))k->UnicodeChar=27;
  if(is("wrong-key"))k->UnicodeChar='U';
  return is("timeout")?EFI_NOT_READY:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI rehearsal_stall(UINTN us){
  if(Gu.retained){assert(us==1000000);longjmp(stopped,1);}
  if(us==100000 && GuLogReady())arming_waits++;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI rehearsal_flush(EFI_FILE_PROTOCOL *f){
  if(is("intent-log-fail") && strstr(report,"REHEARSAL_INTENT:"))return EFI_DEVICE_ERROR;
  if(is("handoff-log-fail") && strstr(report,"windows_handoff=pending"))return EFI_DEVICE_ERROR;
  return flush(f);
}
int main(int argc,char **argv){
  assert(argc==2);scenario=argv[1];setup();
  rt.GetVariable=secure;bs.SetWatchdogTimer=watchdog;bs.Stall=rehearsal_stall;
  con_in.ReadKeyStroke=rehearsal_key;
  for(unsigned i=0;i<4;i++)files[i].p.Flush=rehearsal_flush;
  if(is("wrong-subsystem"))config[11]^=1;
  if(is("wrong-rom"))prom[4000]^=1;
  EFI_STATUS status=EFI_SUCCESS;int halted=setjmp(stopped);
  if(!halted)status=EfiMain(app_handle,&st);
  assert(!mutation_calls && !Gu.mmio_writes && !Gu.flrs && !Gu.dma_count && !Gu.allocations && !Gu.started && !Gu.owned);
  if(mutated){assert(halted && !cancelled);}
  else if(is("cancel-error")){assert(halted && cancelled==1 && !started);}
  else {assert(!halted && status!=EFI_SUCCESS && cancelled==1 && !started);}
  if(is("start-return")||is("start-error")){assert(started==1 && ran==1 && prepared==1 && planned==1);}
  else assert(!started);
  if(is("secure-on")||is("secure-warning")||is("wrong-subsystem")||is("wrong-rom"))assert(!prepared);
  if(is("escape")||is("timeout")||is("wrong-key")||is("intent-log-fail")||is("watchdog-warning"))assert(!ran);
  if(is("timeout")||is("wrong-key"))assert(arming_waits==1200);
  if(is("run-partial")||is("run-warning")||is("run-fault"))assert(!restored);
  assert(pool_allocs==pool_frees);
  printf("entry %s: prepared=%u planned=%u run=%u mutated=%u start=%u halted=%u direct_mutations=0\n",
    scenario,prepared,planned,ran,mutated,started,halted);
}
