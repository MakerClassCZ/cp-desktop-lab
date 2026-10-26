// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

// RGB565 pixels as a PNG in *out (malloc'd): 8-bit RGB, deflate's stored blocks, so no zlib is
// needed. Returns its length, 0 when out of memory.
size_t png_write_rgb565(const uint16_t *pixels, uint16_t width, uint16_t height, uint8_t **out);
