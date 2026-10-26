// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "api_json.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool _reserve(api_buf_t *b, size_t more) {
    if (b->failed) {
        return false;
    }
    if (b->len + more + 1 <= b->cap) {
        return true;
    }
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + more + 1) {
        cap *= 2;
    }
    char *data = realloc(b->data, cap);
    if (data == NULL) {
        b->failed = true;
        return false;
    }
    b->data = data;
    b->cap = cap;
    return true;
}

void api_buf_add(api_buf_t *b, const char *s, size_t n) {
    if (_reserve(b, n)) {
        memcpy(b->data + b->len, s, n);
        b->len += n;
        b->data[b->len] = '\0';
    }
}

void api_buf_printf(api_buf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !_reserve(b, (size_t)n)) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

// The length of the UTF-8 sequence at s, 0 when it is not one.
static size_t _utf8(const uint8_t *s, size_t n) {
    size_t len = s[0] < 0x80 ? 1 : (s[0] >> 5) == 6 ? 2 : (s[0] >> 4) == 14 ? 3 : (s[0] >> 3) == 30 ? 4 : 0;
    if (len == 0 || len > n) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            return 0;
        }
    }
    return len;
}

void api_buf_json_string(api_buf_t *b, const char *s, size_t n) {
    if (s == NULL) {
        api_buf_add(b, "null", 4);
        return;
    }
    api_buf_add(b, "\"", 1);
    const uint8_t *u = (const uint8_t *)s;
    for (size_t i = 0; i < n;) {
        uint8_t c = u[i];
        if (c == '"' || c == '\\') {
            char esc[2] = { '\\', (char)c };
            api_buf_add(b, esc, 2);
            i++;
        } else if (c < 0x20 || c == 0x7f) {
            api_buf_printf(b, "\\u%04x", c);
            i++;
        } else {
            size_t len = _utf8(u + i, n - i);
            if (len == 0) {
                api_buf_add(b, "\\ufffd", 6);
                i++;
            } else {
                api_buf_add(b, s + i, len);
                i += len;
            }
        }
    }
    api_buf_add(b, "\"", 1);
}

void api_buf_free(api_buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
    b->failed = false;
}
