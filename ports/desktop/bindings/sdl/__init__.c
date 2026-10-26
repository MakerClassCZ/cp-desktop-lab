// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/sdl/Framebuffer.h"

//| """Display output on a Linux host through SDL2
//|
//| SDL2 draws in a window on a desktop and full screen through KMS/DRM or fbdev on a handheld.
//| The board builds ``board.DISPLAY`` from an `sdl.Framebuffer`; ``CIRCUITPY_DISPLAY_WIDTH``,
//| ``CIRCUITPY_DISPLAY_HEIGHT``, ``CIRCUITPY_DISPLAY_SCALE``, ``CIRCUITPY_DISPLAY_FULLSCREEN`` and
//| ``CIRCUITPY_DISPLAY_ROTATION`` in ``settings.toml`` change it."""

static const mp_rom_map_elem_t sdl_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sdl) },
    { MP_ROM_QSTR(MP_QSTR_Framebuffer), MP_ROM_PTR(&sdl_framebuffer_type) },
};
static MP_DEFINE_CONST_DICT(sdl_module_globals, sdl_module_globals_table);

const mp_obj_module_t sdl_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&sdl_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_sdl, sdl_module);
