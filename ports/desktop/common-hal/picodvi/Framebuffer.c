// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// picodvi.Framebuffer on the desktop: the HSTX or PIO DVI output of an RP2040 or RP2350 board as
// the board panel's display DVI. Its pins are only claimed; the picture is the framebuffer's, in
// the board's formats: 1 and 2 bits grey, 4 bits RGBI, 8 bits RGB332, 16 bits RGB565, 32 bits
// RGB888.

#include <stdlib.h>
#include <string.h>

#include "py/gc.h"
#include "py/runtime.h"

#include "bindings/picodvi/Framebuffer.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "vdisplay.h"

bool common_hal_picodvi_framebuffer_preflight(mp_uint_t width, mp_uint_t height, mp_uint_t color_depth) {
    return width > 0 && height > 0 && width <= 1280 && height <= 720;
}

void common_hal_picodvi_framebuffer_construct(picodvi_framebuffer_obj_t *self,
    mp_uint_t width, mp_uint_t height,
    const mcu_pin_obj_t *clk_dp, const mcu_pin_obj_t *clk_dn,
    const mcu_pin_obj_t *red_dp, const mcu_pin_obj_t *red_dn,
    const mcu_pin_obj_t *green_dp, const mcu_pin_obj_t *green_dn,
    const mcu_pin_obj_t *blue_dp, const mcu_pin_obj_t *blue_dn,
    mp_uint_t color_depth) {
    if (!common_hal_picodvi_framebuffer_preflight(width, height, color_depth)) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("Invalid %q and %q"), MP_QSTR_width, MP_QSTR_height);
    }
    const mcu_pin_obj_t *pins[] = { clk_dp, clk_dn, red_dp, red_dn, green_dp, green_dn, blue_dp, blue_dn };
    self->width = width;
    self->height = height;
    self->color_depth = color_depth;
    self->pitch = (width * color_depth + 31) / 32;
    self->framebuffer_len = (size_t)self->pitch * height;
    // Outside the VM's heap, as a display outlives the VM that made it.
    self->framebuffer = calloc(self->framebuffer_len, sizeof(uint32_t));
    if (self->framebuffer == NULL) {
        m_malloc_fail(self->framebuffer_len * sizeof(uint32_t));
    }
    self->display = vdisplay_by_name("DVI");
    if (self->display == NULL) {
        self->display = vdisplay_add("DVI", width, height);
    } else if (self->display->width != width || self->display->height != height) {
        vdisplay_resize(self->display, width, height);
    }
    if (self->display == NULL) {
        free(self->framebuffer);
        self->framebuffer = NULL;
        mp_raise_RuntimeError(MP_ERROR_TEXT("too many displays"));
    }
    for (size_t i = 0; i < MP_ARRAY_SIZE(pins); i++) {
        common_hal_mcu_pin_claim(pins[i]);
    }
}

void common_hal_picodvi_framebuffer_deinit(picodvi_framebuffer_obj_t *self) {
    if (self->framebuffer == NULL) {
        return;
    }
    free(self->framebuffer);
    self->framebuffer = NULL;
    if (self->display != NULL) {
        self->display->on = false;
        vdisplay_changed(self->display);
    }
}

bool common_hal_picodvi_framebuffer_deinited(picodvi_framebuffer_obj_t *self) {
    return self->framebuffer == NULL;
}

static uint16_t _rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)((r & 0xf8) << 8 | (g & 0xfc) << 3 | b >> 3);
}

static uint16_t _pixel(const picodvi_framebuffer_obj_t *self, const uint8_t *row, mp_uint_t x) {
    switch (self->color_depth) {
        case 32: {
            uint32_t v = ((const uint32_t *)row)[x];
            return _rgb565(v >> 16, v >> 8, v);
        }
        case 16:
            return ((const uint16_t *)row)[x];
        case 8: {
            uint8_t v = row[x];
            return _rgb565((v >> 5) * 255 / 7, ((v >> 2) & 7) * 255 / 7, (v & 3) * 255 / 3);
        }
        default: {
            // Several pixels a byte, the first in the low bits, as displayio packs them.
            unsigned depth = self->color_depth, per = 8 / depth;
            uint8_t v = (row[x / per] >> ((x % per) * depth)) & ((1 << depth) - 1);
            if (depth == 4) {
                // RGBI: a bright and a dim level of each colour.
                uint8_t level = v & 8 ? 0xff : 0xaa;
                return _rgb565(v & 4 ? level : 0, v & 2 ? level : 0, v & 1 ? level : 0);
            }
            uint8_t grey = v * 255 / ((1 << depth) - 1);
            return _rgb565(grey, grey, grey);
        }
    }
}

void common_hal_picodvi_framebuffer_refresh(picodvi_framebuffer_obj_t *self) {
    vdisplay_t *d = self->display;
    if (self->framebuffer == NULL || d == NULL || d->pixels == NULL) {
        return;
    }
    for (mp_uint_t y = 0; y < self->height && y < d->height; y++) {
        const uint8_t *row = (const uint8_t *)(self->framebuffer + (size_t)y * self->pitch);
        for (mp_uint_t x = 0; x < self->width && x < d->width; x++) {
            d->pixels[y * d->width + x] = _pixel(self, row, x);
        }
    }
    d->on = true;
    vdisplay_changed(d);
}

void common_hal_picodvi_framebuffer_wait_for_vblank(picodvi_framebuffer_obj_t *self) {
}

int common_hal_picodvi_framebuffer_get_width(picodvi_framebuffer_obj_t *self) {
    return self->width;
}

int common_hal_picodvi_framebuffer_get_height(picodvi_framebuffer_obj_t *self) {
    return self->height;
}

int common_hal_picodvi_framebuffer_get_row_stride(picodvi_framebuffer_obj_t *self) {
    return self->pitch * sizeof(uint32_t);
}

int common_hal_picodvi_framebuffer_get_color_depth(picodvi_framebuffer_obj_t *self) {
    return self->color_depth;
}

int common_hal_picodvi_framebuffer_get_native_frames_per_second(picodvi_framebuffer_obj_t *self) {
    return 60;
}

bool common_hal_picodvi_framebuffer_get_grayscale(picodvi_framebuffer_obj_t *self) {
    return self->color_depth < 4;
}

mp_int_t common_hal_picodvi_framebuffer_get_buffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo, mp_uint_t flags) {
    picodvi_framebuffer_obj_t *self = (picodvi_framebuffer_obj_t *)self_in;
    bufinfo->buf = self->framebuffer;
    bufinfo->typecode = self->color_depth == 32 ? 'I' : self->color_depth == 16 ? 'H' : 'B';
    bufinfo->len = self->framebuffer_len * sizeof(uint32_t);
    return 0;
}
