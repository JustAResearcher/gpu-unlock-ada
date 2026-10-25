/* SPDX-License-Identifier: GPL-2.0-only */
/* Reuse the rehearsal EFI/file/PE fixture, but link the actual staged-state
 * guard at the handoff boundary. The Python runner removes only the fixture's
 * production include, logging/restoration stubs, and renames its main. */
#include "../src/rehearsal_handoff.c"
#include "staged_state.h"
#include "gfw_eval.h"
#include "handoff_fixture.h"

static EFI_PCI_IO_PROTOCOL StatePci;
static UINTN StateReads, LogCalls, LateCallbacks;
static BOOLEAN Preparing = TRUE, AfterGuard, GuardWarning, GuardFailure;
static const char *Inject;

VOID GuLog(const char *Format, ...) {
  (void)Format; assert(log_ready); LogCalls++;
}
BOOLEAN GuLogReady(VOID) {return log_ready;}
EFI_STATUS GuRehearsalRestored(VOID) {
  restored_calls++;
  assert(mutated && !log_ready && Gu.started && !Gu.owned);
  if (GuardWarning) return EFI_WARN_UNKNOWN_GLYPH;
  if (GuardFailure) return EFI_NOT_READY;
  EFI_STATUS Status = GuStagedPostState(FALSE);
  AfterGuard = Status == EFI_SUCCESS;
  return Status;
}

static BOOLEAN InjectAt(const char *Name) {
  return Inject && !strcmp(Inject, Name);
}
static EFI_STATUS Warning(const char *Name, EFI_STATUS Status) {
  assert(!AfterGuard); /* Nothing runs between final guard and StartImage. */
  if (!Preparing) LateCallbacks++;
  return Status == EFI_SUCCESS && InjectAt(Name) ? EFI_WARN_UNKNOWN_GLYPH : Status;
}
static EFI_STATUS EFIAPI WarnMap(UINTN *N, EFI_MEMORY_DESCRIPTOR *D, UINTN *Key,
                                 UINTN *Stride, UINT32 *Version) {
  return Warning("map", Map(N, D, Key, Stride, Version));
}
static EFI_STATUS EFIAPI WarnHandle(EFI_HANDLE H, EFI_GUID *G, VOID **P) {
  return Warning("handle", Handle(H, G, P));
}
static EFI_STATUS EFIAPI WarnHandles(EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *Guid,
                                    VOID *Key, UINTN *N, EFI_HANDLE **H) {
  return Warning("enumeration", Handles(Type, Guid, Key, N, H));
}
static EFI_STATUS EFIAPI WarnOpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs,
                                       EFI_FILE_PROTOCOL **Root) {
  return Warning("volume", OpenVolume(Fs, Root));
}
static EFI_STATUS EFIAPI WarnOpen(EFI_FILE_PROTOCOL *Root, EFI_FILE_PROTOCOL **File,
                                 CHAR16 *Path, UINT64 Mode, UINT64 Attrs) {
  return Warning("open", Open(Root, File, Path, Mode, Attrs));
}
static EFI_STATUS EFIAPI WarnInfo(EFI_FILE_PROTOCOL *File, EFI_GUID *Guid,
                                 UINTN *N, VOID *Buffer) {
  return Warning("info", Info(File, Guid, N, Buffer));
}
static EFI_STATUS EFIAPI WarnRead(EFI_FILE_PROTOCOL *File, UINTN *N, VOID *Buffer) {
  return Warning("read", Read(File, N, Buffer));
}
static EFI_STATUS EFIAPI WarnClose(EFI_FILE_PROTOCOL *File) {
  return Warning("close", Close(File));
}
static EFI_STATUS EFIAPI WarnFree(VOID *P) {
  return Warning("free", Release(P));
}
static EFI_STATUS EFIAPI WarnLoad(BOOLEAN Boot, EFI_HANDLE Parent,
  EFI_DEVICE_PATH_PROTOCOL *Path, VOID *Source, UINTN Bytes, EFI_HANDLE *Out) {
  return Warning("load", Load(Boot, Parent, Path, Source, Bytes, Out));
}
static EFI_STATUS EFIAPI StagedStart(EFI_HANDLE H, UINTN *Bytes, CHAR16 **Data) {
  assert(AfterGuard && StateReads == 23 && !log_ready);
  EFI_STATUS Status = Start(H, Bytes, Data);
  return Is("start-warning") ? EFI_WARN_UNKNOWN_GLYPH : Status;
}

static EFI_STATUS EFIAPI StateConfig(EFI_PCI_IO_PROTOCOL *Io,
  EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT32 Offset, UINTN Count, VOID *Out) {
  assert(Io == &StatePci && Count == 1 && !Preparing && !log_ready);
  StateReads++;
  if (Width == EfiPciIoWidthUint32) {
    assert(Offset == 0 || Offset == 0x2cU);
    *(UINT32 *)Out = Offset == 0 ? GU_BOARD_ID : GU_BOARD_SUBSYSTEM;
  } else {
    assert(Width == EfiPciIoWidthUint16 && (Offset == 4 || Offset == 0x7a));
    *(UINT16 *)Out = Offset == 4 ? (Is("late-bme") ? 6U : 2U) : 0;
  }
  return Is("late-pci-warning") ? EFI_WARN_UNKNOWN_GLYPH : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI StateMem(EFI_PCI_IO_PROTOCOL *Io,
  EFI_PCI_IO_PROTOCOL_WIDTH Width, UINT8 Bar, UINT64 Offset, UINTN Count, VOID *Out) {
  assert(Io == &StatePci && Width == EfiPciIoWidthUint32 && Bar == 0 && Count == 1);
  assert(!Preparing && !log_ready); StateReads++;
  switch (Offset) {
    case 0: *(UINT32 *)Out = GU_BOARD_CHIP; break;
    case 0x001183a4U: *(UINT32 *)Out = GU_PROFILE_VRAM; break;
    case 0x001fa7ccU: case 0x001fa7c4U: *(UINT32 *)Out = 0x4cb8fU; break;
    case 0x009a0148U: *(UINT32 *)Out = 0xffffff0fU; break;
    case GU_FEAT_PLM_OFFSET: *(UINT32 *)Out = Is("late-feat") ? 0xffffff8fU : 0xffffffffU; break;
    case GU_FEAT_SS0_OFFSET: *(UINT32 *)Out = Is("late-selector") ? 0U : 0x88888888U; break;
    case GU_FEAT_SS1_OFFSET: *(UINT32 *)Out = 8U; break;
    case GU_FEAT_READOUT_1_OFFSET: *(UINT32 *)Out = Is("late-effective") ? 1U : 0U; break;
    case GU_GFW_PLM_OFFSET: *(UINT32 *)Out = 1U; break;
    case GU_GFW_BOOT_OFFSET: *(UINT32 *)Out = 0xffU; break;
    case GU_GFW_CPUCTL_OFFSET: *(UINT32 *)Out = Is("late-gfw") ? 0U : 0x10U; break;
    case 0x001fa824U: *(UINT32 *)Out = 0U; break;
    case 0x001fa828U: *(UINT32 *)Out = Is("late-wpr2") ? 0x10U : 0U; break;
    default: assert(!"unexpected MMIO");
  }
  return Is("late-mmio-warning") ? EFI_WARN_UNKNOWN_GLYPH : EFI_SUCCESS;
}

static VOID PostTransaction(VOID) {
  Gu.pci = &StatePci; StatePci.Pci.Read = StateConfig; StatePci.Mem.Read = StateMem;
  Gu.bar_base = GU_PROFILE_BAR_BASE; Gu.bar_size = GU_PROFILE_BAR_SIZE; Gu.pcie_cap = 0x70;
  Gu.started = Gu.quiesced = Gu.saved = TRUE; Gu.flrs = 1; Gu.mmio_writes = 200;
  Gu.owned = Gu.armed = FALSE;
}

int main(int Argc, char **Argv) {
  assert(Argc == 2); scenario = Argv[1]; Init();
  bs.GetMemoryMap = WarnMap; bs.HandleProtocol = WarnHandle;
  bs.LocateHandleBuffer = WarnHandles; bs.LoadImage = WarnLoad;
  bs.FreePool = WarnFree; bs.StartImage = StagedStart;
  for (UINTN I = 0; I < 2; I++) {
    a.fs[I].OpenVolume = WarnOpenVolume; a.root[I].Open = WarnOpen; a.root[I].Close = WarnClose;
    a.file[I].Read = WarnRead; a.file[I].GetInfo = WarnInfo; a.file[I].Close = WarnClose;
  }
  if (!strncmp(scenario, "prepare-warning-", 16)) Inject = scenario + 16;
  if (Is("stale-windows")) a.disk[513] = 1; /* Still valid PE, different Windows bytes. */
  EFI_STATUS Status = GuRehearsalWindowsPrepare(&table);
  if (Is("stale-windows") || Is("pin-bytes") || Is("pin-hash") || Inject) {
    assert(Status != EFI_SUCCESS && !starts && !pools && !restored_calls && !StateReads);
    if (Is("stale-windows") || Is("pin-bytes") || Is("pin-hash")) {
      assert(Status == EFI_CRC_ERROR && !loads);
    }
    puts("staged preload refused before mutation: PASS"); return 0;
  }
  assert(Status == EFI_SUCCESS && loads == 1 && pools == 2);
  if (Is("cancel")) {
    assert(GuRehearsalWindowsCancel() == EFI_SUCCESS && !pools && !starts && unloads == 1);
    puts("staged pre-mutation cancel: PASS"); return 0;
  }
  PostTransaction(); mutated = TRUE; Preparing = FALSE;
  /* Explicitly simulate the completed USB close. Nothing logs or reopens a
   * file after this point; state is independently sampled during Start. */
  log_ready = FALSE;
  if (Is("late-graph")) GuardFailure = TRUE;
  if (Is("late-guard-warning")) GuardWarning = TRUE;
  if (Is("late-retained")) Gu.retained = TRUE;
  if (Is("late-dma")) Gu.dma_count = 1;
  if (Is("late-file-replaced")) a.disk[513] = 1; /* Must use the already-loaded, pinned image. */
  if (Is("late-loaded-code")) a.loaded[513] = 1;
  if (Is("late-loaded-path")) a.loaded_path[4]++;
  if (Is("late-media")) a.media[0].id++;
  if (Is("late-log-reopened")) log_ready = TRUE;
  if (Is("late-fault")) Gu.fault = TRUE;
  if (!strncmp(scenario, "start-warning-", 14)) Inject = scenario + 14;
  UINTN PreviousLogs = LogCalls, PreviousReads = reads, PreviousPools = pools;
  Status = GuRehearsalWindowsStart();
  assert(LogCalls == PreviousLogs && reads == PreviousReads && pools == PreviousPools);
  if (Is("success") || Is("late-file-replaced") || Is("start-warning")) {
    assert(starts == 1 && restored_calls == 1 && StateReads == 23 && AfterGuard);
    assert(Status == (Is("start-warning") ? EFI_WARN_UNKNOWN_GLYPH : EFI_ABORTED));
    assert(GuRehearsalWindowsStart() == EFI_NOT_READY && starts == 1);
  } else {
    assert(Status != EFI_SUCCESS && !starts && !AfterGuard);
    if (!Is("late-loaded-code") && !Is("late-loaded-path") && !Is("late-media") &&
        !Is("late-log-reopened") && !Is("late-fault") && !Inject) assert(restored_calls == 1);
  }
  assert(GuRehearsalWindowsCancel() == EFI_ACCESS_DENIED);
  /* Fixture teardown only, not a recovery path in production. */
  AfterGuard = FALSE; Inject = NULL; mutated = FALSE; W.started = FALSE;
  assert(GuRehearsalWindowsCancel() == EFI_SUCCESS && !pools);
  puts("staged silent final guard and one-shot handoff: PASS");
  return 0;
}
