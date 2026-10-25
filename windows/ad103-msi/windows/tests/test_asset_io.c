/* SPDX-License-Identifier: GPL-2.0-only */
/* Production asset reader, hash implementation and logger over read-only host
 * files exposed through fake EFI file protocols. No device services exist. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/app_io.c"

GU_PLATFORM Gu;
GU_ASSET GuAssets[5];
static EFI_FILE_PROTOCOL ParentFile, ReportFile, AssetFiles[5];
static FILE *HostFiles[5];
static UINTN Positions[5], InfoCalls[5], Opened[5], Closed[5];
static VOID *Allocations[8];
static UINTN ActiveAllocations, AllocationCalls, Frees, AssetReads, ReportWrites, ReportFlushes;
static UINTN TargetIndex, LiveFiles, Forbidden;
static const char *Scenario, *AssetDirectory;

static BOOLEAN Is(const char *Name) {return !strcmp(Scenario, Name);}
static BOOLEAN Target(UINTN Index) {return Index == TargetIndex;}
static UINTN FileIndex(EFI_FILE_PROTOCOL *File) {
  for (UINTN I = 0; I < 5; I++) if (File == &AssetFiles[I]) return I;
  assert(!"unexpected asset file"); return 0;
}
BOOLEAN GuSafeToReturn(VOID) {
  return !Gu.retained && !Gu.master_attributes_pending &&
    (!Gu.started || (Gu.quiesced && !Gu.fault));
}
VOID *GuAlloc(size_t Bytes) {
  AllocationCalls++;
  if (Is("allocation-failure") && AllocationCalls == TargetIndex + 1) return NULL;
  for (UINTN I = 0; I < 8; I++) if (!Allocations[I]) {
    VOID *Result = calloc(1, Bytes);
    assert(Result); Allocations[I] = Result; ActiveAllocations++; return Result;
  }
  assert(!"too many asset allocations"); return NULL;
}
VOID GuFree(VOID *Memory) {
  if (!Memory) return;
  for (UINTN I = 0; I < 8; I++) if (Allocations[I] == Memory) {
    free(Memory); Allocations[I] = NULL; ActiveAllocations--; Frees++; return;
  }
  assert(!"unknown or duplicate free");
}
static BOOLEAN SameName(const CHAR16 *A, const CHAR16 *B) {
  while (*A && *A == *B) {A++; B++;}
  return *A == *B;
}
static EFI_STATUS EFIAPI OpenAsset(EFI_FILE_PROTOCOL *Self, EFI_FILE_PROTOCOL **Out,
  CHAR16 *Name, UINT64 Mode, UINT64 Attributes) {
  assert(Self == &ParentFile && Mode == EFI_FILE_MODE_READ && !Attributes);
  *Out = NULL;
  for (UINTN I = 0; I < 5; I++) if (SameName(Name, Specs[I].file)) {
    Opened[I]++;
    if (Target(I) && Is("missing-file")) return EFI_NOT_FOUND;
    if (Target(I) && Is("open-warning-null")) return EFI_WARN_UNKNOWN_GLYPH;
    if (Target(I) && Is("open-success-null")) return EFI_SUCCESS;
    char Path[4096]; const char *Leaf = strrchr(Specs[I].name, '/'); assert(Leaf);
    int Bytes = snprintf(Path, sizeof(Path), "%s/%s", AssetDirectory, Leaf + 1);
    assert(Bytes > 0 && (size_t)Bytes < sizeof(Path));
    HostFiles[I] = fopen(Path, "rb"); assert(HostFiles[I]);
    LiveFiles++; *Out = &AssetFiles[I];
    if (Target(I) && Is("open-warning-handle")) return EFI_WARN_UNKNOWN_GLYPH;
    if (Target(I) && Is("open-error-handle")) return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
  }
  assert(!"unexpected asset name"); return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI AssetInfo(EFI_FILE_PROTOCOL *Self, EFI_GUID *Guid,
                                  UINTN *Bytes, VOID *Buffer) {
  UINTN I = FileIndex(Self); assert(HostFiles[I] && !memcmp(Guid, &InfoGuid, sizeof(*Guid)));
  assert(*Bytes >= sizeof(EFI_FILE_INFO)); InfoCalls[I]++;
  EFI_FILE_INFO *Info = Buffer; memset(Info, 0, sizeof(*Info));
  *Bytes = sizeof(*Info); Info->Size = sizeof(*Info); Info->FileSize = Specs[I].size;
  if (Target(I)) {
    if (Is("info-error")) return EFI_DEVICE_ERROR;
    if (Is("info-warning")) return EFI_WARN_UNKNOWN_GLYPH;
    if (Is("info-small-result")) *Bytes = SIZE_OF_EFI_FILE_INFO - 1;
    if (Is("info-large-result")) *Bytes = 513;
    if (Is("info-small-size")) Info->Size = SIZE_OF_EFI_FILE_INFO - 1;
    if (Is("info-large-size")) Info->Size = *Bytes + 1;
    if (Is("wrong-size")) Info->FileSize++;
    if (Is("directory")) Info->Attribute = EFI_FILE_DIRECTORY;
    if (Is("readonly-success")) Info->Attribute = EFI_FILE_READ_ONLY;
    if (InfoCalls[I] == 2) {
      if (Is("late-info-error")) return EFI_DEVICE_ERROR;
      if (Is("late-info-warning")) return EFI_WARN_UNKNOWN_GLYPH;
      if (Is("file-grew")) Info->FileSize++;
      if (Is("file-shrank")) Info->FileSize--;
      if (Is("became-directory")) Info->Attribute = EFI_FILE_DIRECTORY;
      if (Is("late-info-small-size")) Info->Size = 0;
    }
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI AssetRead(EFI_FILE_PROTOCOL *Self, UINTN *Bytes, VOID *Buffer) {
  UINTN I = FileIndex(Self), Wanted = *Bytes;
  assert(HostFiles[I] && Wanted && Wanted <= 1048576); AssetReads++;
  BOOLEAN EofRead = Positions[I] == Specs[I].size;
  if (EofRead) {
    assert(Wanted == 1);
    if (Target(I) && Is("trailing-byte")) {*(UINT8 *)Buffer = 1; *Bytes = 1; return EFI_SUCCESS;}
    if (Target(I) && Is("eof-error")) return EFI_DEVICE_ERROR;
    if (Target(I) && Is("eof-warning")) return EFI_WARN_UNKNOWN_GLYPH;
  }
  size_t Read = fread(Buffer, 1, Wanted, HostFiles[I]); assert(!ferror(HostFiles[I]));
  *Bytes = (UINTN)Read; Positions[I] += Read;
  if (Target(I) && !EofRead) {
    if (Is("read-error")) return EFI_DEVICE_ERROR;
    if (Is("read-warning")) return EFI_WARN_UNKNOWN_GLYPH;
    if (Is("short-read")) {assert(*Bytes); (*Bytes)--;}
    if (Is("oversized-result")) (*Bytes)++;
    if (Is("hash-mismatch")) ((UINT8 *)Buffer)[0] ^= 1;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI AssetClose(EFI_FILE_PROTOCOL *Self) {
  UINTN I = FileIndex(Self); assert(HostFiles[I] && LiveFiles);
  assert(fclose(HostFiles[I]) == 0); HostFiles[I] = NULL; LiveFiles--; Closed[I]++;
  if (Target(I) && Is("close-error")) return EFI_DEVICE_ERROR;
  if (Target(I) && Is("close-warning")) return EFI_WARN_UNKNOWN_GLYPH;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ForbiddenWrite(EFI_FILE_PROTOCOL *Self, UINTN *Bytes, VOID *Buffer) {
  (void)Self; (void)Bytes; (void)Buffer; Forbidden++; return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI ForbiddenFlush(EFI_FILE_PROTOCOL *Self) {
  (void)Self; Forbidden++; return EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI ReportWrite(EFI_FILE_PROTOCOL *Self, UINTN *Bytes, VOID *Buffer) {
  assert(Self == &ReportFile && Buffer && *Bytes); ReportWrites++;
  if (ReportWrites == TargetIndex + 1) {
    if (Is("log-write-error")) return EFI_DEVICE_ERROR;
    if (Is("log-write-warning")) return EFI_WARN_UNKNOWN_GLYPH;
    if (Is("log-short-write")) (*Bytes)--;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ReportFlush(EFI_FILE_PROTOCOL *Self) {
  assert(Self == &ReportFile); ReportFlushes++;
  if (ReportFlushes == TargetIndex + 1) {
    if (Is("log-flush-error")) return EFI_DEVICE_ERROR;
    if (Is("log-flush-warning")) return EFI_WARN_UNKNOWN_GLYPH;
  }
  return EFI_SUCCESS;
}

int main(int Argc, char **Argv) {
  assert(Argc == 4); Scenario = Argv[1]; AssetDirectory = Argv[2]; TargetIndex = strtoul(Argv[3], NULL, 10);
  assert(TargetIndex < 5);
  Parent = &ParentFile; Report = &ReportFile; GuScreenDetached = TRUE;
  ParentFile.Open = OpenAsset; ReportFile.Write = ReportWrite; ReportFile.Flush = ReportFlush;
  for (UINTN I = 0; I < 5; I++) {
    AssetFiles[I].GetInfo = AssetInfo; AssetFiles[I].Read = AssetRead; AssetFiles[I].Close = AssetClose;
    AssetFiles[I].Write = ForbiddenWrite; AssetFiles[I].Flush = ForbiddenFlush;
  }
  EFI_STATUS Status = GuLoadAssets();
  BOOLEAN Success = Is("success") || Is("readonly-success");
  assert(Success ? Status == EFI_SUCCESS : Status != EFI_SUCCESS);
  assert(!Forbidden && !LiveFiles);
  if (Success) {
    assert(ActiveAllocations == 5 && ReportWrites == 5 && ReportFlushes == 5 && GuLogReady());
    for (UINTN I = 0; I < 5; I++) {
      assert(Opened[I] == 1 && Closed[I] == 1 && InfoCalls[I] == 2);
      assert(GuAssets[I].name == Specs[I].name && GuAssets[I].firmware.size == Specs[I].size);
      assert(GuAssets[I].firmware.data && Positions[I] == Specs[I].size);
    }
    /* The real release function retains buffers across uncertain GPU lifetime. */
    Gu.retained = TRUE; GuReleaseAssets(); assert(ActiveAllocations == 5 && !Frees);
    Gu.retained = FALSE; Gu.started = TRUE; GuReleaseAssets(); assert(ActiveAllocations == 5 && !Frees);
    Gu.quiesced = TRUE;
  } else {
    if (Is("hash-mismatch")) assert(Status == EFI_CRC_ERROR);
    if (Is("trailing-byte") || Is("file-grew") || Is("file-shrank") ||
        Is("became-directory") || !strncmp(Scenario, "info-", 5) ||
        Is("wrong-size") || Is("directory") || Is("late-info-small-size")) {
      if (!strstr(Scenario, "error") && !strstr(Scenario, "warning")) assert(Status == EFI_COMPROMISED_DATA);
    }
    if (Is("short-read") || Is("oversized-result")) assert(Status == EFI_END_OF_FILE);
    if (!strncmp(Scenario, "log-", 4)) assert(!GuLogReady() && ActiveAllocations == TargetIndex + 1);
    else assert(GuLogReady() && ActiveAllocations == TargetIndex);
    for (UINTN I = 0; I < TargetIndex; I++) assert(GuAssets[I].firmware.data && Closed[I] == 1);
    for (UINTN I = TargetIndex + 1; I < 5; I++) assert(!Opened[I] && !GuAssets[I].firmware.data);
  }
  GuReleaseAssets(); assert(!ActiveAllocations);
  GU_ASSET Empty[5] = {0}; assert(!memcmp(GuAssets, Empty, sizeof(Empty)));
  UINTN PreviousFrees = Frees; GuReleaseAssets(); assert(Frees == PreviousFrees);
  printf("asset-io %s target=%zu: PASS\n", Scenario, (size_t)TargetIndex);
  return 0;
}
