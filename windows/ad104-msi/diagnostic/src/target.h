/* SPDX-License-Identifier: MIT */
/* The one card a probe build reports registers and PROM for. The default is
 * the captured MSI AD104. PROBE_TARGET_AD102_FE selects an RTX 4090 Founders
 * Edition whose BAR0 address was read from Windows on the MSI B650 host; the
 * probe refuses MMIO reads if firmware assigned a different address. */
#ifndef AD104_TARGET_H
#define AD104_TARGET_H
#if defined(PROBE_TARGET_AD102_FE)
#define PROBE_DEVICE 0x2684u
#define PROBE_SUBVENDOR 0x10deu
#define PROBE_SUBDEVICE 0x16f4u
#define PROBE_BAR0 0xf5000000u
#define PROBE_CHIP 0x192000a1u
/* Cached option ROM size has not been observed; bound it to fit the report. */
#define PROBE_CACHED_ROM_EXACT 0u
#define PROBE_CACHED_ROM_LIMIT 0x80000u
#define PROBE_NAME L"AD102-FE"
#define PROBE_DIR L"AD102-PROBE"
#define PROBE_TARGET_JSON "\"probe_target\":\"ad102-fe\","
#else
#define PROBE_DEVICE 0x2782u
#define PROBE_SUBVENDOR 0x1462u
#define PROBE_SUBDEVICE 0x5132u
#define PROBE_BAR0 0x80000000u
#define PROBE_CHIP 0x194000a1u
#define PROBE_CACHED_ROM_EXACT 150016u
#define PROBE_CACHED_ROM_LIMIT 150016u
#define PROBE_NAME L"AD104"
#define PROBE_DIR L"AD104-PROBE"
#define PROBE_TARGET_JSON ""
#endif
#endif
