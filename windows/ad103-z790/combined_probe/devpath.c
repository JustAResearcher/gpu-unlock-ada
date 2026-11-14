/* SPDX-License-Identifier: GPL-2.0-only */
#include "devpath.h"
#include <IndustryStandard/Acpi10.h>
#include <stddef.h>

static EFI_GUID PathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
VOID *memcpy(VOID *, const VOID *, size_t);

/* Same two helpers as windows/ad104-msi/checks.c's GuDisplayPath -- generic
 * little-endian field reads, not GPU- or board-specific. */
static UINT16 Read16(const UINT8 *Bytes) { return (UINT16)(Bytes[0] | ((UINT16)Bytes[1] << 8U)); }
static UINT32 Read32(const UINT8 *Bytes) { return (UINT32)Read16(Bytes) | ((UINT32)Read16(Bytes + 2) << 16U); }

/* Same bounds logic as checks.c's NodeLength: a node's own Length field is
 * untrusted until checked against what's actually left to read. */
static UINT16 NodeLength(const UINT8 *Bytes, UINTN Remaining) {
  UINT16 Length;
  if (Remaining < 4) return 0;
  Length = Read16(Bytes + 2);
  if (Length < 4 || Length > Remaining) return 0;
  return Length;
}

UINTN GuCaptureDevicePathFromPointer(const EFI_DEVICE_PATH_PROTOCOL *Path, UINT8 *Buffer, UINTN BufferBytes) {
  if (!Path || !Buffer || !BufferBytes) return 0;
  const UINT8 *Bytes = (const UINT8 *)Path;
  UINTN At = 0, Cap = BufferBytes < GU_DEVPATH_MAX_BYTES ? BufferBytes : GU_DEVPATH_MAX_BYTES;
  for (UINTN Nodes = 0; Nodes < 128; Nodes++) {
    /* We don't know the real remaining length of the firmware's own buffer,
     * only that it must be a well-formed device path terminated by
     * END_ENTIRE_DEVICE_PATH -- so bound "Remaining" by our own copy limit
     * plus headroom, and let NodeLength's own sanity checks catch garbage. */
    UINT16 Length = NodeLength(Bytes + At, (Cap - At) + 4 < 4096 ? (Cap - At) + 4 : 4096);
    if (!Length || At + Length > Cap) return 0;
    memcpy(Buffer + At, Bytes + At, Length);
    BOOLEAN End = Bytes[At] == END_DEVICE_PATH_TYPE && Bytes[At + 1] == END_ENTIRE_DEVICE_PATH_SUBTYPE && Length == 4;
    At += Length;
    if (End) return At;
  }
  return 0; /* too many nodes to be a sane path -- treat as capture failure */
}

UINTN GuCaptureDevicePath(EFI_HANDLE Handle, UINT8 *Buffer, UINTN BufferBytes) {
  EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
  extern EFI_SYSTEM_TABLE *GuProbeTable; /* set by probe_app.c */
  if (!Handle || !GuProbeTable || !GuProbeTable->BootServices) return 0;
  EFI_STATUS s = GuProbeTable->BootServices->HandleProtocol(Handle, &PathGuid, (VOID **)&Path);
  if (EFI_ERROR(s) || !Path) return 0;
  return GuCaptureDevicePathFromPointer(Path, Buffer, BufferBytes);
}

static void DecodeNode(const UINT8 *N, UINT16 Length, GU_LOGLINE Logf) {
  UINT8 Type = N[0], Sub = N[1];
  if (Type == HARDWARE_DEVICE_PATH && Sub == HW_PCI_DP && Length == sizeof(PCI_DEVICE_PATH))
    Logf("    PCI(device=%u,function=%u)", N[5], N[4]);
  else if (Type == ACPI_DEVICE_PATH && Sub == ACPI_DP && Length == sizeof(ACPI_HID_DEVICE_PATH))
    Logf("    ACPI(hid=%#x,uid=%#x)", Read32(N + 4), Read32(N + 8));
  else if (Type == ACPI_DEVICE_PATH && Sub == ACPI_ADR_DP && Length >= 8)
    Logf("    ACPI_ADR(adr0=%#x)", Read32(N + 4));
  else if (Type == MESSAGING_DEVICE_PATH && Sub == MSG_USB_DP && Length == sizeof(USB_DEVICE_PATH))
    Logf("    USB(port=%u,interface=%u)", N[4], N[5]);
  else if (Type == MEDIA_DEVICE_PATH && Sub == MEDIA_FILEPATH_DP)
    Logf("    FILEPATH(%u bytes of UCS-2 name, not decoded here)", (unsigned)Length - 4);
  else if (Type == END_DEVICE_PATH_TYPE && Sub == END_ENTIRE_DEVICE_PATH_SUBTYPE)
    Logf("    END");
  else
    Logf("    node(type=%u,subtype=%u,len=%u) -- not decoded, see hex above", Type, Sub, Length);
}

VOID GuDescribeDevicePath(const char *Label, const UINT8 *Bytes, UINTN Count, GU_LOGLINE Logf) {
  if (!Count) { Logf("%s: capture failed (no path, or walk exceeded bounds)", Label); return; }
  Logf("%s: %u raw bytes", Label, (unsigned)Count);
  for (UINTN At = 0; At < Count; At += 16) {
    UINTN n = Count - At < 16 ? Count - At : 16;
    char hex[3 * 16 + 1]; UINTN h = 0;
    static const char *Alpha = "0123456789abcdef";
    for (UINTN i = 0; i < n; i++) {
      hex[h++] = Alpha[Bytes[At + i] >> 4]; hex[h++] = Alpha[Bytes[At + i] & 0xf]; hex[h++] = ' ';
    }
    hex[h] = 0;
    Logf("  [%#04x] %s", (unsigned)At, hex);
  }
  UINTN At = 0;
  for (UINTN Nodes = 0; Nodes < 128 && At + 4 <= Count; Nodes++) {
    UINT16 Length = NodeLength(Bytes + At, Count - At);
    if (!Length) { Logf("    (node parse stopped -- bad length at offset %#x)", (unsigned)At); break; }
    DecodeNode(Bytes + At, Length, Logf);
    if (Bytes[At] == END_DEVICE_PATH_TYPE) break;
    At += Length;
  }
}
