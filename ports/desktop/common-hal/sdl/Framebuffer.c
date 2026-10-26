// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>

#include "py/runtime.h"
#include "bindings/sdl/Framebuffer.h"
#include "shared-bindings/framebufferio/FramebufferDisplay.h"
#include "shared-bindings/os/__init__.h"
#include "shared-module/displayio/__init__.h"
#include "supervisor/port.h"
#include "supervisor/port_heap.h"
#include "supervisor/shared/safe_mode.h"
#include "supervisor/shared/settings.h"

#include "sdl_host.h"

bool common_hal_sdl_framebuffer_construct(sdl_framebuffer_obj_t *self, mp_uint_t width, mp_uint_t height) {
    self->pixels = port_malloc_zero(width * height * sizeof(uint16_t), false);
    if (self->pixels == NULL) {
        return false;
    }
    self->width = width;
    self->height = height;
    if (!desktop_sdl_show(self->pixels, width, height)) {
        port_free(self->pixels);
        self->pixels = NULL;
        return false;
    }
    return true;
}

bool common_hal_sdl_framebuffer_deinited(sdl_framebuffer_obj_t *self) {
    return self->pixels == NULL;
}

void common_hal_sdl_framebuffer_deinit(sdl_framebuffer_obj_t *self) {
    if (common_hal_sdl_framebuffer_deinited(self)) {
        return;
    }
    desktop_sdl_hide();
    port_free(self->pixels);
    self->pixels = NULL;
}

void common_hal_sdl_framebuffer_refresh(sdl_framebuffer_obj_t *self) {
    desktop_sdl_present();
}

mp_int_t common_hal_sdl_framebuffer_get_buffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo, mp_uint_t flags) {
    sdl_framebuffer_obj_t *self = (sdl_framebuffer_obj_t *)self_in;
    if (self->pixels == NULL) {
        return 1;
    }
    bufinfo->buf = self->pixels;
    bufinfo->len = self->width * self->height * sizeof(uint16_t);
    bufinfo->typecode = 'H';
    return 0;
}

void sdl_framebuffer_autoconstruct(int width, int height, int scale, bool fullscreen) {
    mp_int_t setting = width;
    (void)settings_get_int("CIRCUITPY_DISPLAY_WIDTH", &setting);
    width = setting;
    setting = height;
    (void)settings_get_int("CIRCUITPY_DISPLAY_HEIGHT", &setting);
    height = setting;
    setting = scale;
    (void)settings_get_int("CIRCUITPY_DISPLAY_SCALE", &setting);
    scale = setting;
    (void)settings_get_bool("CIRCUITPY_DISPLAY_FULLSCREEN", &fullscreen);
    mp_int_t rotation = 0;
    (void)settings_get_int("CIRCUITPY_DISPLAY_ROTATION", &rotation);
    if (rotation != 90 && rotation != 180 && rotation != 270) {
        rotation = 0;
    }
    if (width < 16 || height < 16 || width > 4096 || height > 4096) {
        return;
    }
    desktop_sdl_configure(scale, fullscreen);

    sdl_framebuffer_obj_t *fb = &allocate_display_bus()->sdl;
    if (fb == NULL) {
        return;
    }
    fb->base.type = &sdl_framebuffer_type;
    if (!common_hal_sdl_framebuffer_construct(fb, width, height)) {
        fb->base.type = &mp_type_NoneType;
        return;
    }
    framebufferio_framebufferdisplay_obj_t *display = &allocate_display()->framebuffer_display;
    display->base.type = &framebufferio_framebufferdisplay_type;
    common_hal_framebufferio_framebufferdisplay_construct(display, MP_OBJ_FROM_PTR(fb), rotation, true);
}
