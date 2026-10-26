// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "bus_trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"

#include "control_api.h"

#define SHOWN 64                // bytes of one transfer written out; the rest is counted

static FILE *_out;
static bool _api;               // the control API keeps the lines for its readers

bool bus_trace_open(const char *path) {
    // Appended: a reset starts the process again with the same options.
    _out = strcmp(path, "-") == 0 ? stderr : fopen(path, "a");
    if (_out == NULL) {
        perror(path);
        return false;
    }
    return true;
}

void bus_trace_to_api(void) {
    _api = true;
}

bool bus_trace_on(void) {
    return _out != NULL || _api;
}

// The time, then the line, to the file and the API; the file flushed so that a reader sees it at
// once (Windows has no line buffering).
__attribute__((format(printf, 1, 2)))
static void _line(const char *fmt, ...) {
    char line[1024];            // the longest: a NeoPixel's SHOWN colours, or a pulse train
    uint64_t ms = mp_hal_ticks_ms();
    int n = snprintf(line, sizeof(line), "%5lu.%03u ", (unsigned long)(ms / 1000), (unsigned)(ms % 1000));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    if (_out != NULL) {
        fputs(line, _out);
        fflush(_out);
    }
    if (_api) {
        control_api_trace(line, strlen(line));
    }
}

// " 01 02 ab", the first SHOWN bytes and how many more.
static void _hex(char *buf, size_t cap, const uint8_t *data, size_t len) {
    size_t n = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < len && i < SHOWN && n + 4 < cap; i++) {
        n += snprintf(buf + n, cap - n, " %02x", data[i]);
    }
    if (len > SHOWN) {
        snprintf(buf + n, cap - n, " ... (+%u)", (unsigned)(len - SHOWN));
    }
}

static const char *_status(int status) {
    switch (status) {
        case 0:
            return "";
        case -MP_ENODEV:
            return " -> NACK";
        case -MP_EIO:
            return " -> EIO";
        case -MP_ETIMEDOUT:
            return " -> ETIMEDOUT";
        default:
            return " -> error";
    }
}

void bus_trace_i2c(uint16_t address, char dir, const uint8_t *data, size_t len, bool stop, int status) {
    if (!bus_trace_on()) {
        return;
    }
    char hex[SHOWN * 3 + 32];
    _hex(hex, sizeof(hex), data, len);
    if (dir == 'P') {
        _line("I2C 0x%02x probe%s", address, _status(status));
    } else if (dir == 'R') {
        _line("I2C 0x%02x R %u:%s%s", address, (unsigned)len, status == 0 ? hex : "", _status(status));
    } else {
        _line("I2C 0x%02x W%s%s%s", address, hex, stop ? "" : " (no stop)", _status(status));
    }
}

void bus_trace_spi_config(uint32_t hz, uint8_t polarity, uint8_t phase, uint8_t bits) {
    if (bus_trace_on()) {
        _line("SPI %lu Hz mode %u, %u bits", (unsigned long)hz, (unsigned)(polarity * 2 + phase), (unsigned)bits);
    }
}

void bus_trace_spi(const uint8_t *out, const uint8_t *in, size_t len, bool ok) {
    if (!bus_trace_on()) {
        return;
    }
    char sent[SHOWN * 3 + 32];
    char got[SHOWN * 3 + 32];
    _hex(sent, sizeof(sent), out, out != NULL ? len : 0);
    _hex(got, sizeof(got), in, in != NULL && ok ? len : 0);
    const char *end = ok ? "" : " -> EIO";
    if (out != NULL && in != NULL) {
        _line("SPI X %u:%s ->%s%s", (unsigned)len, sent, got, end);
    } else if (out != NULL) {
        _line("SPI W %u:%s%s", (unsigned)len, sent, end);
    } else {
        _line("SPI R %u:%s%s", (unsigned)len, got, end);
    }
}

void bus_trace_neopixel(uint8_t pin, const uint8_t (*rgbw)[4], size_t count, bool white) {
    if (!bus_trace_on()) {
        return;
    }
    char colours[SHOWN * 9 + 32];
    size_t n = 0;
    colours[0] = '\0';
    for (size_t i = 0; i < count && i < SHOWN; i++) {
        n += snprintf(colours + n, sizeof(colours) - n, " %02x%02x%02x", rgbw[i][0], rgbw[i][1], rgbw[i][2]);
        if (white) {
            n += snprintf(colours + n, sizeof(colours) - n, "%02x", rgbw[i][3]);
        }
    }
    if (count > SHOWN) {
        snprintf(colours + n, sizeof(colours) - n, " ... (+%u)", (unsigned)(count - SHOWN));
    }
    _line("NEOPIXEL GPIO%u %u:%s", (unsigned)pin, (unsigned)count, colours);
}

void bus_trace_text(const char *fmt, ...) {
    if (!bus_trace_on()) {
        return;
    }
    char text[600];             // a pulse train is long
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    _line("%s", text);
}

void bus_trace_uart(bool tx, const uint8_t *data, size_t len) {
    if (!bus_trace_on() || len == 0) {
        return;
    }
    char hex[SHOWN * 3 + 32];
    char text[SHOWN + 1];
    _hex(hex, sizeof(hex), data, len);
    size_t n = len < SHOWN ? len : SHOWN;
    for (size_t i = 0; i < n; i++) {
        text[i] = data[i] >= 0x20 && data[i] < 0x7f ? (char)data[i] : '.';
    }
    text[n] = '\0';
    _line("UART %s %u:%s  %s", tx ? "TX" : "RX", (unsigned)len, hex, text);
}
