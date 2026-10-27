// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// NeoPixels: what neopixel_write sends to a pin, kept as a row or a matrix for the board panel
// (panel.c) to draw. A pin nobody laid out is a row as long as the data.

#define NEOPIXEL_EMU_STRIPS 8

// A layout for a pin: cols x rows (a row: rows = 1), serpentine (every other row reversed),
// order: the colour of each byte on the wire, "GRB" (the WS2812's), "RGB", "GRBW" ...
bool neopixel_emu_add(uint8_t pin, uint16_t cols, uint16_t rows, bool serpentine, const char *order);
// "10", "8x8", "16x16 serpentine GRBW": false when it is not a layout.
bool neopixel_emu_parse(const char *spec, uint16_t *cols, uint16_t *rows, bool *serpentine, char order[5]);

void neopixel_emu_write(uint8_t pin, const uint8_t *data, size_t len);
// The strips, for the panel: their layout and colours (R, G, B, W each).
size_t neopixel_emu_strips(void);
// The strip on a pin, its index, and whether it has white; -1 for none.
int neopixel_emu_find(uint8_t pin, bool *rgbw);
bool neopixel_emu_strip(size_t i, uint16_t *cols, uint16_t *rows, bool *serpentine, size_t *count, const uint8_t (**rgbw)[4]);
