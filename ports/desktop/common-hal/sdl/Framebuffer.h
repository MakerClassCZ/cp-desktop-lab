// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

typedef struct {
    mp_obj_base_t base;
    uint16_t *pixels;           // RGB565, native byte order; NULL once deinitialized
    uint16_t width;
    uint16_t height;
} sdl_framebuffer_obj_t;

// board.DISPLAY, from CIRCUITPY_DISPLAY_* in settings.toml or the board's defaults.
void sdl_framebuffer_autoconstruct(int width, int height, int scale, bool fullscreen);
