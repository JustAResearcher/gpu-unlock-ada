/* SPDX-License-Identifier: GPL-2.0-only */
#include "format.h"
#include <stdint.h>

typedef struct {
    char *data;
    size_t capacity;
    size_t used;
} OUTPUT;

static void put(OUTPUT *o, char c)
{
    if (o->used + 1 < o->capacity) {
        o->data[o->used] = c;
    }
    o->used++;
}

size_t FormatV(char *destination, size_t capacity, const char *format, va_list args)
{
    OUTPUT o = { destination, capacity, 0 };
    if (!format || (capacity && !destination)) return SIZE_MAX;

    while (*format) {
        if (*format != '%') {
            put(&o, *format++);
            continue;
        }
        format++;

        unsigned width = 0, length = 0, base = 10;
        int alternate = 0, zero = 0, negative = 0;

        while (*format == '#' || *format == '0') {
            if (*format == '#') alternate = 1;
            else zero = 1;
            format++;
        }

        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (unsigned)(*format++ - '0');
            if (width > 128) return SIZE_MAX;
        }

        if (*format == 'l') {
            length = 1; format++;
            if (*format == 'l') { length = 2; format++; }
        } else if (*format == 'z') {
            length = 3; format++;
        }

        char kind = *format;
        if (!kind) return SIZE_MAX;
        format++;

        if (kind == 's') {
            if (length == 1) { /* %ls: Wide Unicode string (CHAR16 *) */
                const uint16_t *ws = va_arg(args, const uint16_t *);
                if (!ws) ws = (const uint16_t *)u"(null)";
                while (*ws) {
                    put(&o, (char)(*ws++ & 0x7F));
                }
            } else { /* %s: ASCII string (char *) */
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s) put(&o, *s++);
            }
            continue;
        }

        if (kind == 'c' || kind == '%') {
            put(&o, kind == '%' ? '%' : (char)va_arg(args, int));
            continue;
        }

        uint64_t value = 0;
        if (kind == 'd' || kind == 'i') {
            int64_t signed_value;
            if (length == 2) signed_value = va_arg(args, long long);
            else if (length == 1) signed_value = va_arg(args, long);
            else if (length == 3) signed_value = va_arg(args, ptrdiff_t);
            else signed_value = va_arg(args, int);
            negative = signed_value < 0;
            value = negative ? 0 - (uint64_t)signed_value : (uint64_t)signed_value;
        } else if (kind == 'u' || kind == 'x' || kind == 'X') {
            if (length == 2) value = va_arg(args, unsigned long long);
            else if (length == 1) value = va_arg(args, unsigned long);
            else if (length == 3) value = va_arg(args, size_t);
            else value = va_arg(args, unsigned int);
            if (kind != 'u') base = 16;
        } else {
            return SIZE_MAX;
        }

        char digits[64];
        unsigned digit_count = 0;
        const char *glyph = (kind == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";

        do {
            digits[digit_count++] = glyph[value % base];
            value /= base;
        } while (value);

        unsigned prefix_len = (negative ? 1 : 0) + (alternate && base == 16 ? 2 : 0);
        unsigned total_len = digit_count + prefix_len;
        unsigned pad = (width > total_len) ? (width - total_len) : 0;

        if (!zero) {
            while (pad--) put(&o, ' ');
        }
        if (negative) put(&o, '-');
        if (alternate && base == 16) {
            put(&o, '0');
            put(&o, kind == 'X' ? 'X' : 'x');
        }
        if (zero) {
            while (pad--) put(&o, '0');
        }
        while (digit_count) {
            put(&o, digits[--digit_count]);
        }
    }

    if (o.capacity) {
        if (o.used < o.capacity) o.data[o.used] = '\0';
        else o.data[o.capacity - 1] = '\0';
    }
    return o.used;
}
