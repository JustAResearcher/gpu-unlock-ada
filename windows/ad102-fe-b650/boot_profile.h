/* SPDX-License-Identifier: GPL-2.0-only */
/* Keep the local ESP identifier and boot-manager pin out of the public tree. */
#pragma once
#include "local/boot_profile.h"
#if defined(GU_HOST_TEST_PROFILE)
#undef GU_WINDOWS_PARTITION_INITIALIZER
#define GU_WINDOWS_PARTITION_INITIALIZER {0x11111111,0x2222,0x3333,{0x84,0x44,0x55,0x66,0x77,0x88,0x99,0xaa}}
#endif
#if !defined(GU_WINDOWS_PARTITION_INITIALIZER)||!defined(GU_WINDOWS_BOOTMGR_BYTES)||!defined(GU_WINDOWS_BOOTMGR_SHA256)
#error "Configure the reviewed ESP GUID and bootmgfw.efi pin in local/boot_profile.h before building."
#endif
#if GU_WINDOWS_BOOTMGR_BYTES<1048576||GU_WINDOWS_BOOTMGR_BYTES>8388608
#error "GU_WINDOWS_BOOTMGR_BYTES must be the reviewed size of the ESP copy of bootmgfw.efi."
#endif
