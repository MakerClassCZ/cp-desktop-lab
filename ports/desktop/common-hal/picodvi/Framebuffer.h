// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "py/obj.h"

#include "vdisplay.h"

// A DVI output as a display of the board panel: the framebuffer in the program's format,
// converted to the panel's RGB565 at each refresh.
typedef struct {
    mp_obj_base_t base;
    uint32_t *framebuffer;      // NULL once deinited
    size_t framebuffer_len;     // in words
    mp_uint_t width, height;
    uint16_t pitch;             // words from one row to the next
    uint8_t color_depth;
    vdisplay_t *display;
} picodvi_framebuffer_obj_t;
