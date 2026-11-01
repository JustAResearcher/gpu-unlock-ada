/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UNIVERSAL_FORMAT_H
#define UNIVERSAL_FORMAT_H

#include <stddef.h>
#include <stdarg.h>

size_t FormatV(char *destination, size_t capacity, const char *format, va_list args);

#endif /* UNIVERSAL_FORMAT_H */

