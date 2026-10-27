// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "neopixel_emu.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus_trace.h"
#include "panel.h"

#define MAX_PIXELS 4096

typedef struct {
    uint8_t pin;
    uint16_t cols, rows;
    bool automatic;             // nobody laid it out: a row as long as the data
    bool serpentine;
    char order[5];
    uint8_t bpp;
    size_t count;
    uint8_t(*rgbw)[4];
} strip_t;

static strip_t _strips[NEOPIXEL_EMU_STRIPS];
static size_t _strip_count;

bool neopixel_emu_parse(const char *spec, uint16_t *cols, uint16_t *rows, bool *serpentine, char order[5]) {
    *cols = 0;
    *rows = 1;
    *serpentine = false;
    strcpy(order, "GRB");
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", spec);
    bool sized = false;
    for (char *word = strtok(buf, " \t,"); word != NULL; word = strtok(NULL, " \t,")) {
        unsigned w, h;
        char rest;
        if (sscanf(word, "%ux%u%c", &w, &h, &rest) == 2) {
            *cols = (uint16_t)w;
            *rows = (uint16_t)h;
            sized = true;
        } else if (sscanf(word, "%u%c", &w, &rest) == 1) {
            *cols = (uint16_t)w;
            *rows = 1;
            sized = true;
        } else if (strcmp(word, "serpentine") == 0 || strcmp(word, "zigzag") == 0) {
            *serpentine = true;
        } else {
            size_t n = strlen(word);
            bool ok = n == 3 || n == 4;
            for (size_t i = 0; ok && i < n; i++) {
                char c = (char)toupper((unsigned char)word[i]);
                ok = strchr(n == 3 ? "RGB" : "RGBW", c) != NULL && memchr(word, word[i], i) == NULL;
                order[i] = c;
            }
            if (!ok) {
                return false;
            }
            order[n] = '\0';
        }
    }
    return sized && *cols > 0 && *rows > 0 && (size_t)*cols * *rows <= MAX_PIXELS;
}

static strip_t *_find(uint8_t pin) {
    for (size_t i = 0; i < _strip_count; i++) {
        if (_strips[i].pin == pin) {
            return &_strips[i];
        }
    }
    return NULL;
}

bool neopixel_emu_add(uint8_t pin, uint16_t cols, uint16_t rows, bool serpentine, const char *order) {
    strip_t *s = _find(pin);
    if (s == NULL) {
        if (_strip_count == NEOPIXEL_EMU_STRIPS) {
            return false;
        }
        s = &_strips[_strip_count++];
    }
    s->pin = pin;
    s->cols = cols;
    s->rows = rows;
    s->serpentine = serpentine;
    s->automatic = false;
    snprintf(s->order, sizeof(s->order), "%s", order);
    s->bpp = (uint8_t)strlen(s->order);
    s->count = (size_t)cols * rows;
    free(s->rgbw);
    s->rgbw = calloc(s->count > 0 ? s->count : 1, 4);
    return s->rgbw != NULL;
}

void neopixel_emu_write(uint8_t pin, const uint8_t *data, size_t len) {
    strip_t *s = _find(pin);
    if (s == NULL || s->automatic) {
        // Nobody laid it out: a row as long as the data, the WS2812's byte order.
        size_t n = len / (s != NULL ? s->bpp : 3);
        if (n > MAX_PIXELS) {
            n = MAX_PIXELS;
        }
        if (s == NULL || s->count != n) {
            if (!neopixel_emu_add(pin, (uint16_t)(n > 0 ? n : 1), 1, false, s != NULL ? s->order : "GRB")) {
                return;
            }
            s = _find(pin);
            s->automatic = true;
        }
    }
    size_t n = len / s->bpp;
    if (n > s->count) {
        n = s->count;
    }
    for (size_t p = 0; p < n; p++) {
        uint8_t px[4] = { 0, 0, 0, 0 };
        for (uint8_t k = 0; k < s->bpp; k++) {
            px[strchr("RGBW", s->order[k]) - "RGBW"] = data[p * s->bpp + k];
        }
        memcpy(s->rgbw[p], px, 4);
    }
    bus_trace_neopixel(pin, (const uint8_t(*)[4])s->rgbw, n, s->bpp == 4);
    panel_changed();
}

int neopixel_emu_find(uint8_t pin, bool *rgbw) {
    strip_t *s = _find(pin);
    if (s == NULL) {
        return -1;
    }
    *rgbw = s->bpp == 4;
    return (int)(s - _strips);
}

size_t neopixel_emu_strips(void) {
    return _strip_count;
}

bool neopixel_emu_strip(size_t i, uint16_t *cols, uint16_t *rows, bool *serpentine, size_t *count, const uint8_t (**rgbw)[4]) {
    if (i >= _strip_count) {
        return false;
    }
    *cols = _strips[i].cols;
    *rows = _strips[i].rows;
    *serpentine = _strips[i].serpentine;
    *count = _strips[i].count;
    *rgbw = (const uint8_t(*)[4])_strips[i].rgbw;
    return true;
}
