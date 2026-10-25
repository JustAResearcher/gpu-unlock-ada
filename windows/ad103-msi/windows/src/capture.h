/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "port.h"
/* Observational only: never calls a driver binding or modifies GPU/boot state. */
EFI_STATUS GuCaptureFirmware(EFI_SYSTEM_TABLE *Table);
