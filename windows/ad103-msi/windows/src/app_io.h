/* SPDX-License-Identifier: GPL-2.0-only */
/* Adapted 2026-10-24 for gpu-unlock-ada; see ../THIRD_PARTY_NOTICES.md. */
#pragma once
#include "port.h"
EFI_STATUS GuIoOpen(EFI_HANDLE Image,EFI_SYSTEM_TABLE *Table);
EFI_STATUS GuIoClose(VOID);
EFI_STATUS GuLoadAssets(VOID);
EFI_STATUS GuIoConsumeSession(VOID);
VOID GuReleaseAssets(VOID);
BOOLEAN GuLogReady(VOID);
extern BOOLEAN GuScreenDetached;
VOID GuConsole(const char *Text);
/* Diagnostic-only, file-only metadata capture. No console output or
 * driver-binding callbacks. File-service callbacks write the USB capture.
 * Write accepts NUL-terminated chunks of at most 8192 bytes; total cap 2 MiB.
 * Errors close the capture and remain available from GuCaptureClose(). */
EFI_STATUS GuCaptureOpen(VOID);
EFI_STATUS GuCaptureWrite(const char *Text);
EFI_STATUS GuCaptureClose(VOID);
