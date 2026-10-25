/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* The shipped default cannot arm or write GPU registers. An experimental
 * build must include a profile generated from a completed diagnostic run. */
#ifndef GU_ENABLE_UNLOCK
#define GU_ENABLE_UNLOCK 0
#endif
#ifndef GU_REHEARSAL
#define GU_REHEARSAL 0
#endif
#ifndef GU_STAGED_UNLOCK
#define GU_STAGED_UNLOCK 0
#endif
#if GU_STAGED_UNLOCK && (!GU_ENABLE_UNLOCK || GU_REHEARSAL)
#error Staged unlock requires an isolated explicit unlock build
#endif
#if GU_REHEARSAL && GU_ENABLE_UNLOCK
#error Rehearsal cannot include the unlock transaction
#endif
#define GU_BOARD_ID 0x270510deU
#define GU_PROM_BAR_OFFSET 0x300000U
#define GU_PROM_VIEW_BYTES 0x100000U
#if GU_ENABLE_UNLOCK || GU_REHEARSAL
#include "board_profile.h"
#if GU_PROFILE_VERSION != 1 || GU_PROFILE_ID != GU_BOARD_ID
#error Unsupported diagnostic profile
#endif
#else
#define GU_BOARD_SUBSYSTEM 0U
#define GU_BOARD_CHIP 0U
#define GU_PROFILE_SEGMENT 0U
#define GU_PROFILE_BUS 0U
#define GU_PROFILE_DEVICE 0U
#define GU_PROFILE_FUNCTION 0U
#define GU_PROFILE_BAR_BASE 0ULL
#define GU_PROFILE_BAR_SIZE 0ULL
#define GU_PROFILE_PROM_HASH ""
#define GU_PROFILE_ROM_HASH ""
#define GU_PROFILE_ROM_SIZE 0U
#define GU_PROFILE_VRAM 0U
#define GU_PROFILE_TAG "diagnostic-only"
#endif
