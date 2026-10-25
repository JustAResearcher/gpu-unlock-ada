/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GPU_UNLOCK_DIAGNOSTIC_H
#define GPU_UNLOCK_DIAGNOSTIC_H
#if defined(GU_NATIVE_WINDOWS)
#include <ntddk.h>
#define GU_NOTE(fmt, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL, \
                                    "gpu_unlock: " fmt, ##__VA_ARGS__)
#define GU_ERROR(fmt, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, \
                                     "gpu_unlock: " fmt, ##__VA_ARGS__)
#elif defined(__KERNEL__)
#include <linux/printk.h>
#define GU_NOTE(fmt, ...) pr_info("gpu_unlock: " fmt, ##__VA_ARGS__)
#define GU_ERROR(fmt, ...) pr_err("gpu_unlock: " fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define GU_NOTE(fmt, ...) fprintf(stderr, "gpu_unlock: " fmt, ##__VA_ARGS__)
#define GU_ERROR(fmt, ...) fprintf(stderr, "gpu_unlock: " fmt, ##__VA_ARGS__)
#endif

/* No extra device reads. Preserve the errno and identify the exact guard in
 * the matching source, including pure parser failures. */
static inline int gu_diagnostic_result(int error, const char *file,
                                      const char *function, unsigned int line)
{
    if (error)
        GU_ERROR("failure %s:%u %s error=%d\n", file, line, function, error);
    return error;
}
#define GU_DIAG(result) gu_diagnostic_result((result), __FILE__, __func__, __LINE__)
#endif
