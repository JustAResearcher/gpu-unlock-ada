/* SPDX-License-Identifier: GPL-2.0-only */
/* Copy to local/boot_profile.h. Enter the reviewed ESP partition GUID and the
 * size and SHA-256 of \EFI\Microsoft\Boot\bootmgfw.efi read from that ESP.
 * The zero values are deliberately unusable. Windows updates replace
 * bootmgfw.efi; re-pin it after an update instead of removing the check.
 * Other layout, GPU, ROM and driver-code guards still have to match the
 * exact profile. Do not remove or widen guards to make another machine boot. */
#define GU_WINDOWS_PARTITION_INITIALIZER {0,0,0,{0,0,0,0,0,0,0,0}}
#define GU_WINDOWS_BOOTMGR_BYTES 0
#define GU_WINDOWS_BOOTMGR_SHA256 "0000000000000000000000000000000000000000000000000000000000000000"
