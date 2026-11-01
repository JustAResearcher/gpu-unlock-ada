/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIVERSAL_UNLOCK_ENGINE_H
#define UNIVERSAL_UNLOCK_ENGINE_H

#include "gpu_types.h"

/**
 * VOLATILE IN-RAM GPU UNLOCK ENGINE (Zero SPI Flash Writes)
 *
 * SAFETY GUARANTEE:
 * 1. This engine NEVER accesses or writes to physical SPI Flash ROM (0% risk of bricking).
 * 2. All operations are executed purely in host RAM buffers and GPU Falcon IMEM/DMEM registers.
 * 3. Latches performance profile (SS0/SS1: 0x88888888, 0x8) and triggers FLR (Function Level Reset).
 * 4. All changes are volatile: if the system restarts without this loader, the GPU is 100% stock factory.
 *
 * Supports:
 * - Ampere (RTX 30 series, GA10x)
 * - Ada Lovelace (RTX 40 series, AD10x)
 */
EFI_STATUS ExecuteGpuRamUnlock(DETECTED_GPU *gpu);

#endif /* UNIVERSAL_UNLOCK_ENGINE_H */

