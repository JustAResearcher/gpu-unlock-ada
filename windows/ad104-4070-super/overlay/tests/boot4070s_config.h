/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Synthetic host identities only; real GUIDs and paths belong in local/. */
#define GU_HOST_TEST_PROFILE 1
#include "../boot_profile.h"
#ifndef GU_ASSET_PARTITION_INITIALIZER
#define GU_ASSET_PARTITION_INITIALIZER {0xabcdef01,0x2345,0x4678,{0x89,0xab,0xcd,0xef,0x12,0x34,0x56,0x78}}
#endif
#ifndef GU_GSP_PATH
#define GU_GSP_PATH L"\\Users\\HostFixture\\gsp_ga10x.bin"
#endif
