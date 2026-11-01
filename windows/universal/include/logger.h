/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIVERSAL_LOGGER_H
#define UNIVERSAL_LOGGER_H

#include "uefi_base.h"

/**
 * Initializes file logging to USB drive under \AD104-UNLOCK\RUNxxxx.TXT.
 * Automatically searches for next available RUNxxxx.TXT index.
 */
EFI_STATUS LoggerInit(EFI_HANDLE image_handle);

/**
 * Formats and writes log messages to both USB log file and screen.
 */
VOID LogWrite(CONST CHAR8 *format, ...);

/**
 * Closes the log file before Windows boot.
 */
VOID LoggerClose(VOID);

#endif /* UNIVERSAL_LOGGER_H */

