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

#include "desktop_port.h"
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
    bool sized = settings_get_int("CIRCUITPY_DISPLAY_WIDTH", &setting) == SETTINGS_OK;
    width = setting;
    setting = height;
    sized |= settings_get_int("CIRCUITPY_DISPLAY_HEIGHT", &setting) == SETTINGS_OK;
    // settings.toml's size wins over a board file's [board] display: said, as it is easy to miss.
    if (sized && desktop_display_sized && (width != desktop_display_width || setting != desktop_display_height)) {
        fprintf(stderr, "circuitpython: settings.toml's display %dx%d over the board file's %dx%d\r\n", width, (int)setting,
            desktop_display_width, desktop_display_height);
    }
    height = setting;
    setting = scale;
    (void)settings_get_int("CIRCUITPY_DISPLAY_SCALE", &setting);
    scale = setting;
    (void)settings_get_bool("CIRCUITPY_DISPLAY_FULLSCREEN", &fullscreen);
    mp_int_t rotation = 0;
    (void)settings_get_int("CIRCUITPY_DISPLAY_ROTATION", &rotation);
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
        fprintf(stderr, "circuitpython: settings.toml: CIRCUITPY_DISPLAY_ROTATION %d is not 0, 90, 180 or 270; 0 it is\r\n",
            (int)rotation);
        rotation = 0;
    }
    if (width < 16 || height < 16 || width > 4096 || height > 4096) {
        fprintf(stderr, "circuitpython: settings.toml: a display of %dx%d is not 16 to 4096 each; no board.DISPLAY\r\n",
            width, height);
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
