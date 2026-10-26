// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>

// A growing text buffer for the control API's answers (control_api.c), JSON mostly.

typedef struct {
    char *data;
    size_t len, cap;
    bool failed;                // out of memory: the answer is dropped
} api_buf_t;

void api_buf_add(api_buf_t *b, const char *s, size_t n);
__attribute__((format(printf, 2, 3)))
void api_buf_printf(api_buf_t *b, const char *fmt, ...);
// s as a JSON string, quotes included; NULL as null. Bytes that are not UTF-8 become U+FFFD.
void api_buf_json_string(api_buf_t *b, const char *s, size_t n);
void api_buf_free(api_buf_t *b);
