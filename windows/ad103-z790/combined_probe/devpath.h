/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only device-path capture: given a handle, get its
 * EFI_DEVICE_PATH_PROTOCOL and hex-dump the raw bytes plus a short
 * per-node decode, bounded and defensive throughout (this walks whatever
 * the firmware hands back, not data we control). Never writes anything;
 * callers decide where the resulting text goes. */
#pragma once
#include <Uefi.h>
#include <Protocol/DevicePath.h>

#define GU_DEVPATH_MAX_BYTES 4096U

/* Copies up to GU_DEVPATH_MAX_BYTES of Path into Buffer, stopping at the
 * first END_ENTIRE_DEVICE_PATH node or at the first malformed/oversized
 * node. Returns the number of bytes copied (0 on failure); never overruns
 * Buffer. Use this directly when you already hold a device-path pointer
 * (e.g. EFI_LOADED_IMAGE_PROTOCOL.FilePath). */
UINTN GuCaptureDevicePathFromPointer(const EFI_DEVICE_PATH_PROTOCOL *Path, UINT8 *Buffer, UINTN BufferBytes);

/* Looks up EFI_DEVICE_PATH_PROTOCOL on Handle via HandleProtocol (a plain
 * interface lookup -- does not open or change anything), then calls
 * GuCaptureDevicePathFromPointer. Returns 0 if the handle has no device
 * path protocol installed. */
UINTN GuCaptureDevicePath(EFI_HANDLE Handle, UINT8 *Buffer, UINTN BufferBytes);

/* Writes Bytes as a hex dump, 16 bytes per Logf-style line via the
 * caller-supplied callback, prefixed with Label. Also writes a short
 * per-node decode line (Type/SubType/Length, and for a few recognized
 * node types the fields checks.c already knows how to read: ACPI HID/UID,
 * PCI Device/Function, ACPI _ADR, USB port number). */
typedef void (*GU_LOGLINE)(const char *Format, ...);
VOID GuDescribeDevicePath(const char *Label, const UINT8 *Bytes, UINTN Count, GU_LOGLINE Logf);
