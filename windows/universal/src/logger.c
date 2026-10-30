/* SPDX-License-Identifier: GPL-2.0-only */
#include "logger.h"
#include "format.h"

static EFI_GUID LoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID SimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

static EFI_FILE_HANDLE g_LogFile = NULL;
static CHAR16 g_LogPath[64] = { 0 };

EFI_STATUS LoggerInit(EFI_HANDLE image_handle)
{
    if (g_LogFile) return EFI_ALREADY_STARTED;

    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    EFI_STATUS s = gBS->HandleProtocol(image_handle, &LoadedImageProtocolGuid, (VOID **)&loaded);
    if (EFI_ERROR(s) || !loaded || !loaded->DeviceHandle) return s;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    s = gBS->HandleProtocol(loaded->DeviceHandle, &SimpleFileSystemProtocolGuid, (VOID **)&fs);
    if (EFI_ERROR(s) || !fs) return s;

    EFI_FILE_HANDLE root = NULL;
    s = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(s) || !root) return s;

    /* Strictly universal logging directory: \UNLOCK-LOGS\ */
    EFI_FILE_HANDLE dir = NULL;
    s = root->Open(root, &dir, L"\\UNLOCK-LOGS",
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                   EFI_FILE_DIRECTORY);

    if (EFI_ERROR(s) || !dir) {
        root->Close(root);
        return s;
    }

    /* Find next free RUNxxxx.TXT inside \UNLOCK-LOGS\ */
    for (UINTN slot = 1; slot <= 9999; slot++) {
        CHAR16 name[] = L"RUN0000.TXT";
        UINTN number = slot;
        for (UINTN at = 7; at > 3; at--) {
            name[at] = (CHAR16)(L'0' + (number % 10));
            number /= 10;
        }

        EFI_FILE_HANDLE existing = NULL;
        s = dir->Open(dir, &existing, name, EFI_FILE_MODE_READ, 0);
        if (!EFI_ERROR(s) && existing) {
            existing->Close(existing);
            continue; /* File exists, check next slot */
        }

        /* Found unused slot! Create it for writing */
        s = dir->Open(dir, &g_LogFile, name,
                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
        if (!EFI_ERROR(s) && g_LogFile) {
            for (UINTN i = 0; i < 12; i++) g_LogPath[i] = name[i];
            break;
        }
    }

    dir->Close(dir);
    root->Close(root);

    if (g_LogFile) {
        LogWrite("=== UNIVERSAL MULTI-GPU UNLOCK LOG START ===\n");
    }

    return g_LogFile ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static VOID ConsolePrintAscii(CONST CHAR8 *str, size_t len)
{
    if (!gST || !gST->ConOut || !gST->ConOut->OutputString) return;

    CHAR16 wide_buf[256];
    size_t w_idx = 0;

    for (size_t i = 0; i < len; i++) {
        CHAR8 c = str[i];
        if (c == '\r') {
            continue; /* Normalize \r\n line endings */
        }
        if (c == '\n') {
            if (w_idx + 2 >= ARRAY_SIZE(wide_buf)) {
                wide_buf[w_idx] = L'\0';
                gST->ConOut->OutputString(gST->ConOut, wide_buf);
                w_idx = 0;
            }
            wide_buf[w_idx++] = L'\r';
            wide_buf[w_idx++] = L'\n';
        } else {
            if (w_idx + 1 >= ARRAY_SIZE(wide_buf)) {
                wide_buf[w_idx] = L'\0';
                gST->ConOut->OutputString(gST->ConOut, wide_buf);
                w_idx = 0;
            }
            wide_buf[w_idx++] = (CHAR16)(UINT8)c;
        }
    }
    if (w_idx > 0) {
        wide_buf[w_idx] = L'\0';
        gST->ConOut->OutputString(gST->ConOut, wide_buf);
    }
}

VOID LogWrite(CONST CHAR8 *format, ...)
{
    CHAR8 buffer[2048];
    va_list args;
    va_start(args, format);
    size_t len = FormatV(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (len == (size_t)-1 || len >= sizeof(buffer)) return;

    /* Write full log to screen console in real-time */
    ConsolePrintAscii(buffer, len);

    /* Write to log file on USB */
    if (g_LogFile) {
        UINTN bytes = len;
        g_LogFile->Write(g_LogFile, &bytes, buffer);
        g_LogFile->Flush(g_LogFile);
    }
}

VOID GuLog(const char *format, ...)
{
    CHAR8 buffer[2048];
    va_list args;
    va_start(args, format);
    size_t len = FormatV(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (len == (size_t)-1 || len >= sizeof(buffer)) return;

    /* Write full log to screen console in real-time */
    ConsolePrintAscii(buffer, len);

    if (g_LogFile) {
        UINTN bytes = len;
        g_LogFile->Write(g_LogFile, &bytes, buffer);
        g_LogFile->Flush(g_LogFile);
    }
}

VOID GuLogText(const char *text)
{
    GuLog("%s", text);
}

VOID LoggerClose(VOID)
{
    if (g_LogFile) {
        LogWrite("=== LOG CLOSED: TRANSITIONING TO WINDOWS ===\n");
        g_LogFile->Flush(g_LogFile);
        g_LogFile->Close(g_LogFile);
        g_LogFile = NULL;
    }
}
