// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --trace-bus PATH: every busio transfer as one line, seconds since start first. "-" = stderr.
bool bus_trace_open(const char *path);
bool bus_trace_on(void);
// The lines go to the control API's trace stream too (--api).
void bus_trace_to_api(void);

// dir: 'W' write, 'R' read, 'P' probe. status: 0 or -MP_E*.
void bus_trace_i2c(uint16_t address, char dir, const uint8_t *data, size_t len, bool stop, int status);
void bus_trace_spi_config(uint32_t hz, uint8_t polarity, uint8_t phase, uint8_t bits);
// out or in may be NULL (a read or a write); ok = the transfer completed.
void bus_trace_spi(const uint8_t *out, const uint8_t *in, size_t len, bool ok);
void bus_trace_uart(bool tx, const uint8_t *data, size_t len);
// Any other line: the board panel's parts.
__attribute__((format(printf, 1, 2)))
void bus_trace_text(const char *fmt, ...);
// The colours neopixel_write gave a pin: rrggbb each, rrggbbww with white.
void bus_trace_neopixel(uint8_t pin, const uint8_t (*rgbw)[4], size_t count, bool white);
