/* SPDX-License-Identifier: GPL-2.0-only */
/* Copy to local/boot_profile.h and replace with the reviewed ESP partition GUID.
 * All-zero is deliberately not a usable configuration. Other layout, binary
 * hash, GPU, ROM and driver-code guards still have to match the exact profile.
 * Do not remove or widen those guards to make another machine boot. */
#define GU_WINDOWS_PARTITION_INITIALIZER {0,0,0,{0,0,0,0,0,0,0,0}}
