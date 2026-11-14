/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AD103/Z790 combined diagnostic probe -- READ ONLY.
 *
 * Supersedes ../probe/ and ../ownership_probe/: does both captures those two
 * separate .EFIs did, in one boot, to one log file. They were always
 * disjoint, non-overlapping, read-only snapshots of the same one-time
 * machine state (the register/VBIOS probe never walked the protocol-
 * ownership graph; the ownership probe never read PCI config space or BAR0),
 * so there was never a reason for them to need separate boots. Combining
 * them halves the number of cold boots the discovery phase needs (each
 * probe's findings are only trusted after >=2 independently confirmed full
 * cold boots -- see ../README.md and ../SS1_FINDINGS.md for
 * why a single capture, or a warm-resumed one, is not enough).
 *
 * What it captures, in order:
 *   1. PCI config space (64 dwords) and a fixed table of BAR0 registers
 *      (including the SS0/SS1 selector pair), plus a live hash of the
 *      BAR0+0x300000 VBIOS aperture checked against the pinned PROM hash.
 *   2. The GPU's and the physical GOP's device paths, the keyboard USB host
 *      controller's device path, and the identity (image base/size, file
 *      path, loaded-code hashes) of the three UEFI drivers bound to the
 *      console-output, GOP and PCI-IO protocols -- everything a board's
 *      windows/<board>/ release module needs real captured values for.
 *
 * It never writes to PCI config space or BAR0, never performs FLR, never
 * allocates DMA, never opens any protocol exclusively or BY_DRIVER, never
 * calls DisconnectController/ConnectController, never touches the internal
 * disk, the ESP, or bootmgfw.efi, and never starts Windows itself -- it only
 * reads and returns to the firmware boot menu when done, every time, whether
 * it succeeds or fails. GuWrite32/GuFlr/GuEnableMaster/GuDmaAllocate and the
 * selector-latch functions from port.c do not exist anywhere in this tree
 * (see port_probe.h/.c, copied unmodified from probe/), so there is no code
 * path that could do any of that even by mistake.
 */
#include "port_probe.h"
#include "checks_probe.h"
#include "graph.h"
#include "devpath.h"
#include "pehash.h"
#include "format.h"
#include "sha256.h"
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/SimpleTextIn.h>

/* From GuAd103PromMatches() in profile.c -- see that file's header comment
 * for why this must come from a live pre-driver EFI read, not an OS-level
 * dump tool's output. */
#define GU_PROM_EXPECTED_HASH "1a6631d6317739908aa6fb246858306dfef15901b43237b35b73d424b3880d9e"
#define GU_PROM_OFFSET 0x300000U
#define GU_PROM_BYTES 1048576U

EFI_SYSTEM_TABLE *GuProbeTable; /* used by devpath.c's HandleProtocol lookups */
static EFI_FILE_PROTOCOL *LogFile;
static BOOLEAN LogOk;
static UINT32 RunIndex;

/* Small label builders -- GuFormatV takes a va_list, not direct arguments,
 * so it isn't usable for this; both truncate rather than overrun Out. */
static VOID LabelS(char *Out, UINTN OutBytes, const char *Prefix, const char *Suffix) {
  UINTN len = 0;
  for (UINTN i = 0; Prefix[i] && len + 1 < OutBytes; i++) Out[len++] = Prefix[i];
  for (UINTN i = 0; Suffix[i] && len + 1 < OutBytes; i++) Out[len++] = Suffix[i];
  Out[len] = 0;
}
static VOID LabelN(char *Out, UINTN OutBytes, const char *Prefix, UINTN N, const char *Suffix) {
  UINTN len = 0;
  for (UINTN i = 0; Prefix[i] && len + 1 < OutBytes; i++) Out[len++] = Prefix[i];
  char digits[24]; UINTN d = 0, v = N;
  do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v && d < sizeof(digits));
  while (d && len + 1 < OutBytes) Out[len++] = digits[--d];
  for (UINTN i = 0; Suffix[i] && len + 1 < OutBytes; i++) Out[len++] = Suffix[i];
  Out[len] = 0;
}

static VOID Say(const CHAR16 *s) {
  if (GuProbeTable && GuProbeTable->ConOut) GuProbeTable->ConOut->OutputString(GuProbeTable->ConOut, (CHAR16 *)s);
}

/* Writes one line to the console and, if the log file is open, to it too --
 * flushed immediately, same reasoning as boot_io.c's GuLog: a long walk has
 * many places to hang, so nothing waits until the end to be saved. */
static VOID Logf(const char *fmt, ...) {
  char line[400];
  va_list args;
  va_start(args, fmt);
  size_t n = GuFormatV(line, sizeof(line), fmt, args);
  va_end(args);
  if (n == (size_t)-1) { Say(L"(a log line was too long for its buffer and was dropped)\r\n"); return; }

  static CHAR16 echo[404];
  UINTN j = 0;
  for (size_t i = 0; line[i] && j + 1 < sizeof(echo) / sizeof(echo[0]); i++) echo[j++] = (CHAR16)(unsigned char)line[i];
  echo[j] = 0;
  Say(echo);
  Say(L"\r\n");

  if (LogOk && LogFile) {
    static CHAR16 wide[404];
    UINTN k = 0;
    for (size_t i = 0; line[i] && k + 2 < sizeof(wide) / sizeof(wide[0]); i++) wide[k++] = (CHAR16)(unsigned char)line[i];
    wide[k++] = L'\r'; wide[k++] = L'\n';
    UINTN bytes = k * sizeof(CHAR16);
    if (EFI_ERROR(LogFile->Write(LogFile, &bytes, wide)) || EFI_ERROR(LogFile->Flush(LogFile))) {
      LogOk = FALSE;
      Say(L"(log write failed -- continuing to console only)\r\n");
    }
  }
}

/* Opens the EFI_FILE_PROTOCOL root of the volume this .EFI was loaded from --
 * never the internal disk/ESP, only wherever this image itself is running
 * from (the USB stick). Used fresh by OpenLog() and by WriteBin() below
 * (OpenLog's own Root handle is closed once the log file is open). */
static EFI_STATUS OpenBootVolumeRoot(EFI_HANDLE Image, EFI_BOOT_SERVICES *Bs, EFI_FILE_PROTOCOL **Root) {
  EFI_GUID LoadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_GUID FsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
  EFI_LOADED_IMAGE_PROTOCOL *LoadedImage = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
  EFI_STATUS s = Bs->HandleProtocol(Image, &LoadedImageGuid, (VOID **)&LoadedImage);
  if (EFI_ERROR(s) || !LoadedImage || !LoadedImage->DeviceHandle) { Say(L"no loaded-image device handle\r\n"); return EFI_NOT_FOUND; }
  s = Bs->HandleProtocol(LoadedImage->DeviceHandle, &FsGuid, (VOID **)&Fs);
  if (EFI_ERROR(s) || !Fs) { Say(L"no filesystem on boot volume\r\n"); return EFI_NOT_FOUND; }
  s = Fs->OpenVolume(Fs, Root);
  if (EFI_ERROR(s) || !*Root) { Say(L"could not open boot volume root\r\n"); return EFI_NOT_FOUND; }
  return EFI_SUCCESS;
}

/* Finds the lowest n (0..999) not already used by AD103-COMBINED-<n>.TXT,
 * opens that name for the main log, and records n in RunIndex so the log
 * itself can tell the owner how many capture runs exist on this stick so
 * far -- a plain cold-boot-confirmation counter, since >=2 independently
 * confirmed cold boots are required before trusting any single value. */
static VOID OpenLog(EFI_HANDLE Image, EFI_BOOT_SERVICES *Bs) {
  EFI_FILE_PROTOCOL *Root = NULL;
  if (EFI_ERROR(OpenBootVolumeRoot(Image, Bs, &Root))) return;
  CHAR16 name[48];
  UINT32 n;
  for (n = 0; n < 1000; n++) {
    UINTN len = 0; CHAR16 digits[8]; UINTN d = 0, v = n;
    do { digits[d++] = (CHAR16)(L'0' + v % 10); v /= 10; } while (v && d < 8);
    const CHAR16 *prefix = L"AD103-COMBINED-"; UINTN p = 0; while (prefix[p]) name[len++] = prefix[p++];
    while (d) name[len++] = digits[--d];
    const CHAR16 *suffix = L".TXT"; UINTN q = 0; while (suffix[q]) name[len++] = suffix[q++];
    name[len] = 0;
    EFI_FILE_PROTOCOL *existing = NULL;
    EFI_STATUS probe = Root->Open(Root, &existing, name, EFI_FILE_MODE_READ, 0);
    if (probe == EFI_NOT_FOUND) break;
    if (!EFI_ERROR(probe)) existing->Close(existing);
  }
  RunIndex = n;
  EFI_STATUS s = Root->Open(Root, &LogFile, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  Root->Close(Root);
  if (EFI_ERROR(s) || !LogFile) { Say(L"could not create log file\r\n"); return; }
  LogOk = TRUE;
  Say(L"Log: "); Say(name); Say(L"\r\n");
}

/* Writes a raw binary buffer to AD103-PROM-<n>.BIN -- used only when the live
 * PROM hash doesn't match, so the actual bytes can be compared offline.
 * Shares AD103-PROM-<n>'s numbering with nothing else, so it can't collide
 * with the main AD103-COMBINED-<n>.TXT log sequence. */
static VOID WriteBin(EFI_HANDLE Image, EFI_BOOT_SERVICES *Bs, const VOID *Data, UINTN Bytes) {
  EFI_FILE_PROTOCOL *Root = NULL, *File = NULL;
  if (EFI_ERROR(OpenBootVolumeRoot(Image, Bs, &Root))) return;
  CHAR16 name[48];
  for (UINT32 n = 0; n < 1000; n++) {
    UINTN len = 0; CHAR16 digits[8]; UINTN d = 0, v = n;
    do { digits[d++] = (CHAR16)(L'0' + v % 10); v /= 10; } while (v && d < 8);
    const CHAR16 *prefix = L"AD103-PROM-"; UINTN p = 0; while (prefix[p]) name[len++] = prefix[p++];
    while (d) name[len++] = digits[--d];
    const CHAR16 *suffix = L".BIN"; UINTN q = 0; while (suffix[q]) name[len++] = suffix[q++];
    name[len] = 0;
    EFI_FILE_PROTOCOL *existing = NULL;
    EFI_STATUS probe = Root->Open(Root, &existing, name, EFI_FILE_MODE_READ, 0);
    if (probe == EFI_NOT_FOUND) break;
    if (!EFI_ERROR(probe)) existing->Close(existing);
  }
  EFI_STATUS s = Root->Open(Root, &File, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR(s) || !File) { Logf("prom: could not create capture file"); Root->Close(Root); return; }
  UINTN wrote = Bytes;
  s = File->Write(File, &wrote, (VOID *)Data);
  File->Close(File);
  Root->Close(Root);
  if (EFI_ERROR(s) || wrote != Bytes) { Logf("prom: capture write failed"); return; }
  Logf("Raw PROM capture written to the stick (see console/log name above)");
}

/* Finds the RTX 4070 Ti SUPER (10de:2705) via PCI IO protocol enumeration and
 * returns both its bus location (for a human-readable log line) and its
 * handle (needed later for the device-path/ownership-graph lookups) --
 * ../probe/'s Discover() and ../ownership_probe/'s DiscoverGpu() merged into
 * one walk instead of two. */
static EFI_STATUS Discover(EFI_BOOT_SERVICES *Bs, UINTN *Segment, UINTN *Bus, UINTN *Device, UINTN *Function, EFI_HANDLE *PciHandle) {
  EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
  EFI_HANDLE *h = NULL;
  EFI_PCI_IO_PROTOCOL *io = NULL;
  UINTN count = 0, found = 0;
  EFI_STATUS s = Bs->LocateHandleBuffer(ByProtocol, &PciGuid, NULL, &count, &h);
  if (s == EFI_SUCCESS && (!h || !count || count > 1024)) s = EFI_UNSUPPORTED;
  for (UINTN i = 0; s == EFI_SUCCESS && i < count; i++) {
    EFI_PCI_IO_PROTOCOL *p = NULL;
    UINT32 id = 0;
    s = Bs->HandleProtocol(h[i], &PciGuid, (VOID **)&p);
    if (s != EFI_SUCCESS || !p || !p->Pci.Read) { s = EFI_UNSUPPORTED; break; }
    s = p->Pci.Read(p, EfiPciIoWidthUint32, 0, 1, &id);
    if (s == EFI_SUCCESS && id == GU_BOARD_ID) { found++; io = p; *PciHandle = h[i]; }
  }
  if (h) Bs->FreePool(h);
  if (s != EFI_SUCCESS || found != 1 || !io) return EFI_UNSUPPORTED;
  if (!io->GetLocation || EFI_ERROR(io->GetLocation(io, Segment, Bus, Device, Function))) return EFI_UNSUPPORTED;
  Gu.pci = io;
  return EFI_SUCCESS;
}

static VOID DumpDriver(const char *Label, EFI_HANDLE Agent, EFI_BOOT_SERVICES *Bs) {
  if (!Agent) { Logf("%s_agent: not identified (0 or >1 BY_DRIVER edge -- see graph dump)", Label); return; }
  Logf("%s_agent handle=%#llx", Label, (unsigned long long)(UINTN)Agent);
  EFI_GUID BindingGuid = EFI_DRIVER_BINDING_PROTOCOL_GUID;
  EFI_DRIVER_BINDING_PROTOCOL *Binding = NULL;
  if (EFI_ERROR(Bs->HandleProtocol(Agent, &BindingGuid, (VOID **)&Binding)) || !Binding) {
    Logf("%s: no EFI_DRIVER_BINDING_PROTOCOL on this agent handle", Label);
    return;
  }
  Logf("%s: binding version=%#x driver_binding_handle=%#llx image_handle=%#llx stop=%#llx",
       Label, Binding->Version, (unsigned long long)(UINTN)Binding->DriverBindingHandle,
       (unsigned long long)(UINTN)Binding->ImageHandle, (unsigned long long)(UINTN)Binding->Stop);
  EFI_GUID ImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_LOADED_IMAGE_PROTOCOL *Image = NULL;
  if (EFI_ERROR(Bs->HandleProtocol(Binding->ImageHandle, &ImageGuid, (VOID **)&Image)) || !Image) {
    Logf("%s: no EFI_LOADED_IMAGE_PROTOCOL on its ImageHandle", Label);
    return;
  }
  UINTN StopOffset = Binding->Stop ? (UINTN)Binding->Stop - (UINTN)Image->ImageBase : 0;
  Logf("%s: image_base=%#llx image_size=%llu code_type=%u device_handle=%#llx stop_offset=%#x",
       Label, (unsigned long long)(UINTN)Image->ImageBase, (unsigned long long)Image->ImageSize, Image->ImageCodeType,
       (unsigned long long)(UINTN)Image->DeviceHandle, (unsigned)StopOffset);

  UINT8 pathbuf[GU_DEVPATH_MAX_BYTES];
  UINTN pathlen = GuCaptureDevicePathFromPointer(Image->FilePath, pathbuf, sizeof(pathbuf));
  char label[64];
  LabelS(label, sizeof(label), Label, "_file_path");
  if (pathlen) GuDescribeDevicePath(label, pathbuf, pathlen, Logf);
  else Logf("%s_file_path: capture failed (FilePath pointer=%#llx)", Label, (unsigned long long)(UINTN)Image->FilePath);

  if (!Image->ImageBase || !Image->ImageSize) { Logf("%s: no readable image bytes", Label); return; }
  char whole_hash[65];
  if (GuHashImageRange(Image->ImageBase, Image->ImageSize, 0, Image->ImageSize, whole_hash))
    Logf("%s: whole_image_sha256=%s bytes=%llu", Label, whole_hash, (unsigned long long)Image->ImageSize);

  GU_PE_INFO pe;
  GuParsePeHeaders(Image->ImageBase, Image->ImageSize, &pe);
  if (!pe.parsed) { Logf("%s: PE header did not parse -- no per-section hash", Label); return; }
  Logf("%s: PE32%s machine=%#x sections=%u", Label, pe.is_pe32_plus ? "+" : "", pe.machine, (unsigned)pe.section_count);
  for (UINTN i = 0; i < pe.section_count; i++) {
    GU_PE_SECTION *sec = &pe.sections[i];
    Logf("%s: section[%u] name=%s va=%#x size=%#x characteristics=%#x code=%u",
         Label, (unsigned)i, sec->name, sec->virtual_address, sec->virtual_size, sec->characteristics, sec->looks_like_code);
    if (sec->looks_like_code && sec->virtual_size) {
      char sec_hash[65];
      if (GuHashImageRange(Image->ImageBase, Image->ImageSize, sec->virtual_address, sec->virtual_size, sec_hash))
        Logf("%s: section[%u] sha256=%s", Label, (unsigned)i, sec_hash);
    }
  }
}

/* The exact offsets ../profile.c's Registers() reads, plus the selector pair
 * (0x82381c/0x823820). Printed as-is; never gated on an expected value here
 * -- see ../profile.c and ../SS1_FINDINGS.md for why SS0/SS1 specifically
 * must never be pinned to a literal in this probe or anywhere upstream of a
 * live capture. */
static const UINT32 RegisterOffsets[] = {
    0, 0x118128, 0x118234, 0x110100, 0x1fa824, 0x1fa828, 0x1fa7cc, 0x1fa7c4,
    0x9a0148, 0x823804, 0x823814, 0x823818, 0x1183a4, 0x625f04, 0x1fa7c8,
    0x1fa82c, 0x1fa830, 0x824148, 0x8241e0, 0x82381c, 0x823820};

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image, EFI_SYSTEM_TABLE *Table) {
  GuProbeTable = Table;
  if (!Image || !Table || !Table->BootServices) return EFI_INVALID_PARAMETER;
  EFI_BOOT_SERVICES *Bs = Table->BootServices;

  Say(L"\r\nAD103/Z790 combined diagnostic probe -- read only, no GPU writes, no driver detach.\r\n");
  OpenLog(Image, Bs);
  Logf("probe started");
  Logf("this is combined capture run #%u on this stick (0-indexed) -- a single run is NOT"
       " enough to trust any register or graph value; confirm across >=2 independently"
       " confirmed full cold boots (true power off ~1 min, HiberbootEnabled=0) first,"
       " same discipline as SS1_FINDINGS.md", RunIndex);
  Logf("console handles: out=%#llx in=%#llx err=%#llx (informational, not stable across boots)",
       (unsigned long long)(UINTN)Table->ConsoleOutHandle, (unsigned long long)(UINTN)Table->ConsoleInHandle,
       (unsigned long long)(UINTN)Table->StandardErrorHandle);

  UINTN seg = 0, bus = 0, dev = 0, fun = 0;
  EFI_HANDLE PciHandle = NULL;
  if (EFI_ERROR(Discover(Bs, &seg, &bus, &dev, &fun, &PciHandle))) {
    Logf("device 10de:2705 (GU_BOARD_ID=%#x) not found via PCI IO protocol enumeration -- stopping", GU_BOARD_ID);
    goto done;
  }
  Logf("found 10de:2705 at segment=%u bus=%u device=%u function=%u handle=%#llx", seg, bus, dev, fun,
       (unsigned long long)(UINTN)PciHandle);

  /* -- part 1: PCI config space, BAR0 registers, VBIOS PROM hash -- */

  GuInit(Bs, Gu.pci);

  UINT32 c[64];
  BOOLEAN ok = TRUE;
  UINTN failedAt = 0;
  for (UINTN i = 0; i < 64 && ok; i++) {
    UINT32 v = 0;
    if (EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32, (UINT32)i * 4, &v))) { ok = FALSE; failedAt = i; }
    c[i] = v;
  }
  if (!ok) {
    Logf("config-space read failed at dword %u", (unsigned)failedAt);
  } else {
    for (UINTN i = 0; i < 64; i++) Logf("config[%#x]=0x%x", (unsigned)(i * 4), c[i]);
  }

  GU_CAPS caps = {0, 0, 0, 0, 0};
  BOOLEAN capsOk = ok && GuCheckConfigProbe(c, seg, dev, fun, &caps);
  Logf("GuCheckConfigProbe=%u pm=0x%x pcie=0x%x pcie_version=%u msi=0x%x msix=0x%x",
       capsOk, caps.pm, caps.pcie, caps.pcie_version, caps.msi, caps.msix);
  if (!capsOk) Logf("WARNING: capability walk or cold-state checks failed -- card may not be in the expected pre-driver state. Registers below are still printed for the record.");

  Gu.fault = FALSE; /* config-space checks use their own fault tracking; clear before BAR reads */
  for (UINTN i = 0; i < sizeof(RegisterOffsets) / sizeof(RegisterOffsets[0]); i++) {
    UINT32 off = RegisterOffsets[i];
    UINT32 v = GuRead32(off);
    Logf("bar0[0x%x]=0x%x%s", off, v, Gu.fault ? " (FAULT -- Mem.Read failed, stopping further BAR reads)" : "");
    if (Gu.fault) break;
  }

  if (Gu.fault) {
    Logf("prom: skipped, BAR reads faulted");
  } else {
    VOID *prom = NULL;
    EFI_STATUS s = Bs->AllocatePool(EfiLoaderData, GU_PROM_BYTES, &prom);
    if (EFI_ERROR(s) || !prom) {
      Logf("prom: could not allocate %u bytes", GU_PROM_BYTES);
    } else {
      UINTN done = 0;
      BOOLEAN readOk = TRUE;
      while (done < GU_PROM_BYTES && readOk) {
        UINTN count = GU_PROM_BYTES - done < 1024 ? GU_PROM_BYTES - done : 1024;
        if (EFI_ERROR(Gu.pci->Mem.Read(Gu.pci, EfiPciIoWidthUint8, 0, GU_PROM_OFFSET + done, count, (UINT8 *)prom + done)))
          readOk = FALSE;
        done += count;
      }
      if (!readOk) {
        Logf("prom: read FAULT, no hash taken");
      } else {
        UINT8 digest[32];
        GuSha256(prom, GU_PROM_BYTES, digest);
        char hex[65];
        for (UINTN i = 0; i < 32; i++) {
          static const char *alpha = "0123456789abcdef";
          hex[i * 2] = alpha[digest[i] >> 4];
          hex[i * 2 + 1] = alpha[digest[i] & 0xf];
        }
        hex[64] = 0;
        const UINT8 *b = (const UINT8 *)prom;
        Logf("prom_first_bytes=0x%x 0x%x 0x%x 0x%x", b[0], b[1], b[2], b[3]);
        Logf("prom_sha256=%s", hex);
        Logf("prom_expected=%s", GU_PROM_EXPECTED_HASH);
        BOOLEAN match = GuHashMatches(prom, GU_PROM_BYTES, GU_PROM_EXPECTED_HASH);
        Logf("prom_match=%u", match);
        if (!match) {
          Logf("prom: MISMATCH, saving the raw 1 MiB read to the USB stick for comparison");
          WriteBin(Image, Bs, prom, GU_PROM_BYTES);
        }
      }
      Bs->FreePool(prom);
    }
  }

  /* -- part 2: protocol-ownership graph -- */

  {
    UINT8 pathbuf[GU_DEVPATH_MAX_BYTES];
    UINTN n = GuCaptureDevicePath(PciHandle, pathbuf, sizeof(pathbuf));
    GuDescribeDevicePath("gpu_pci_device_path", pathbuf, n, Logf);
  }

  EFI_GUID GopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  EFI_HANDLE *GopHandles = NULL;
  UINTN GopCount = 0;
  EFI_HANDLE PhysicalGop = NULL;
  UINTN PhysicalCount = 0;
  EFI_STATUS GopStatus = Bs->LocateHandleBuffer(ByProtocol, &GopGuid, NULL, &GopCount, &GopHandles);
  if (GopStatus == EFI_SUCCESS && GopCount && GopCount <= 16) {
    for (UINTN i = 0; i < GopCount; i++) {
      EFI_GRAPHICS_OUTPUT_PROTOCOL *g = NULL;
      if (EFI_ERROR(Bs->HandleProtocol(GopHandles[i], &GopGuid, (VOID **)&g)) || !g) continue;
      BOOLEAN physical = g->Mode && g->Mode->FrameBufferBase && g->Mode->FrameBufferSize;
      Logf("GOP handle[%u]=%#llx physical=%u", (unsigned)i, (unsigned long long)(UINTN)GopHandles[i], physical);
      if (physical && g->Mode->Info)
        Logf("  resolution=%ux%u pixel_format=%u framebuffer=%#llx size=%#llx",
             g->Mode->Info->HorizontalResolution, g->Mode->Info->VerticalResolution, g->Mode->Info->PixelFormat,
             (unsigned long long)(UINTN)g->Mode->FrameBufferBase, (unsigned long long)g->Mode->FrameBufferSize);
      UINT8 pathbuf[GU_DEVPATH_MAX_BYTES];
      char label[32];
      LabelN(label, sizeof(label), "GOP_device_path[", i, "]");
      UINTN n = GuCaptureDevicePath(GopHandles[i], pathbuf, sizeof(pathbuf));
      GuDescribeDevicePath(label, pathbuf, n, Logf);
      if (physical) { PhysicalGop = GopHandles[i]; PhysicalCount++; }
    }
  } else {
    Logf("GOP enumeration: status=%#llx count=%zu (not walked -- 0 or implausibly many)", (unsigned long long)GopStatus, (size_t)GopCount);
  }
  if (GopHandles) Bs->FreePool(GopHandles);
  if (PhysicalCount != 1) { Logf("physical GOP handles found: %zu (need exactly 1 to identify out/gfx agents)", (size_t)PhysicalCount); PhysicalGop = NULL; }

  /* USB host controller device path, located the same structural way
   * msi_release.c's UsbBaseline does: the EFI_USB2_HC_PROTOCOL_GUID edge
   * where the controller manages itself (BY_DRIVER, controller==handle).
   * This GUID value is the standard UEFI spec constant, not board-specific. */
  EFI_GUID UsbHcGuid = {0x3e745226, 0x9818, 0x45b6, {0xa2, 0xac, 0xd7, 0xcd, 0x0e, 0x8b, 0xa2, 0xbc}};

  GU_GRAPH Graph;
  EFI_STATUS GraphStatus = GuSnapshotGraph(Bs, &Graph);
  Logf("graph snapshot: status=%#llx edges=%zu", (unsigned long long)GraphStatus, (size_t)Graph.count);
  if (GraphStatus != EFI_SUCCESS) Logf("WARNING: graph snapshot incomplete -- the rest of this log reflects a partial view");

  UINTN UsbHostCandidates = 0;
  for (UINTN i = 0; i < Graph.count; i++) {
    GU_EDGE *e = &Graph.edges[i];
    if (GuGuidEqual(&e->protocol, &UsbHcGuid) && e->attributes == 0x10 && e->controller == e->handle) {
      UsbHostCandidates++;
      Logf("usb_host candidate[%zu] handle=%#llx agent=%#llx", UsbHostCandidates - 1,
           (unsigned long long)(UINTN)e->handle, (unsigned long long)(UINTN)e->agent);
      UINT8 pathbuf[GU_DEVPATH_MAX_BYTES];
      char label[48];
      LabelN(label, sizeof(label), "usb_host_device_path[", UsbHostCandidates - 1, "]");
      UINTN n = GuCaptureDevicePath(e->handle, pathbuf, sizeof(pathbuf));
      GuDescribeDevicePath(label, pathbuf, n, Logf);
    }
  }
  if (!UsbHostCandidates) Logf("usb_host: no self-managed EFI_USB2_HC_PROTOCOL_GUID edge found");

  EFI_GUID PciGuid = EFI_PCI_IO_PROTOCOL_GUID;
  EFI_GUID TextGuid = EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
  EFI_HANDLE NvAgent = NULL, OutAgent = NULL, GfxAgent = NULL;
  UINTN NvCount = GuCountEdges(&Graph, PciHandle, &PciGuid, 0x10, &NvAgent);
  Logf("nv_agent: %zu BY_DRIVER edge(s) on the GPU's PCI_IO protocol", NvCount);
  if (PhysicalGop) {
    UINTN OutCount = GuCountEdges(&Graph, PhysicalGop, &TextGuid, 0x10, &OutAgent);
    UINTN GfxCount = GuCountEdges(&Graph, PhysicalGop, &GopGuid, 0x10, &GfxAgent);
    Logf("out_agent: %zu BY_DRIVER edge(s) on the physical GOP's SimpleTextOut protocol", OutCount);
    Logf("gfx_agent: %zu BY_DRIVER edge(s) on the physical GOP's GOP protocol", GfxCount);
  } else {
    Logf("out_agent/gfx_agent: skipped (no single physical GOP handle)");
  }

  DumpDriver("nv", NvAgent, Bs);
  DumpDriver("out", OutAgent, Bs);
  DumpDriver("gfx", GfxAgent, Bs);

  Logf("full graph dump follows (%zu edges)", (size_t)Graph.count);
  for (UINTN i = 0; i < Graph.count; i++) {
    GU_EDGE *e = &Graph.edges[i];
    Logf("edge[%zu] handle=%#llx agent=%#llx controller=%#llx protocol=%#x attributes=%#x count=%u",
         i, (unsigned long long)(UINTN)e->handle, (unsigned long long)(UINTN)e->agent,
         (unsigned long long)(UINTN)e->controller, e->protocol.Data1, e->attributes, e->count);
  }

done:
  Logf("probe finished (run #%u). Nothing was written to the GPU, PCI config space, disk, ESP, or"
       " any driver; no protocol was opened, connected or disconnected.", RunIndex);
  if (LogFile) { LogFile->Flush(LogFile); LogFile->Close(LogFile); }
  Say(L"\r\nDone. Press any key to return to the boot menu.\r\n");
  UINTN idx = 0;
  Bs->WaitForEvent(1, &Table->ConIn->WaitForKey, &idx);
  return EFI_SUCCESS;
}
