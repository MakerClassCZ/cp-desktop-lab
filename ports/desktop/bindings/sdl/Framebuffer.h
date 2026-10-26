// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/sdl/Framebuffer.h"

extern const mp_obj_type_t sdl_framebuffer_type;

// False when there is no memory for the pixels or SDL has no display.
bool common_hal_sdl_framebuffer_construct(sdl_framebuffer_obj_t *self, mp_uint_t width, mp_uint_t height);
void common_hal_sdl_framebuffer_deinit(sdl_framebuffer_obj_t *self);
bool common_hal_sdl_framebuffer_deinited(sdl_framebuffer_obj_t *self);
void common_hal_sdl_framebuffer_refresh(sdl_framebuffer_obj_t *self);
mp_int_t common_hal_sdl_framebuffer_get_buffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo, mp_uint_t flags);
