// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/sdl/Framebuffer.h"
#include "sdl_host.h"
#include "shared-bindings/util.h"
#include "shared-module/displayio/__init__.h"
#include "shared-module/framebufferio/__init__.h"
#include "shared-module/framebufferio/FramebufferDisplay.h"

//| class Framebuffer:
//|     """An RGB565 picture shown in the host's window or on its screen through SDL2.
//|
//|     The board builds one for ``board.DISPLAY``; constructing another one replaces what the
//|     window shows."""
//|
//|     def __init__(self, width: int, height: int) -> None:
//|         """Create a Framebuffer of the given size, in pixels. It is not drawn on by itself:
//|         pass it to `framebufferio.FramebufferDisplay`, or write the pixels through the buffer
//|         protocol (16-bit RGB565 in the machine's byte order).
//|
//|         :param int width: the width in pixels
//|         :param int height: the height in pixels"""
static mp_obj_t sdl_framebuffer_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_width, ARG_height };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_width, MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_height, MP_ARG_INT | MP_ARG_REQUIRED },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_uint_t width = mp_arg_validate_int_range(args[ARG_width].u_int, 1, 4096, MP_QSTR_width);
    mp_uint_t height = mp_arg_validate_int_range(args[ARG_height].u_int, 1, 4096, MP_QSTR_height);

    sdl_framebuffer_obj_t *self = &allocate_display_bus_or_raise()->sdl;
    self->base.type = &sdl_framebuffer_type;
    if (!common_hal_sdl_framebuffer_construct(self, width, height)) {
        self->base.type = &mp_type_NoneType;
        mp_raise_RuntimeError(MP_ERROR_TEXT("No display"));
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Free the pixels and blank the window."""
//|         ...
//|
static mp_obj_t sdl_framebuffer_deinit(mp_obj_t self_in) {
    common_hal_sdl_framebuffer_deinit(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sdl_framebuffer_deinit_obj, sdl_framebuffer_deinit);

static sdl_framebuffer_obj_t *_checked(mp_obj_t self_in) {
    sdl_framebuffer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (common_hal_sdl_framebuffer_deinited(self)) {
        raise_deinited_error();
    }
    return self;
}

//|     width: int
//|     """The width in pixels."""
static mp_obj_t sdl_framebuffer_get_width(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(_checked(self_in)->width);
}
MP_DEFINE_CONST_FUN_OBJ_1(sdl_framebuffer_get_width_obj, sdl_framebuffer_get_width);
MP_PROPERTY_GETTER(sdl_framebuffer_width_obj, (mp_obj_t)&sdl_framebuffer_get_width_obj);

//|     height: int
//|     """The height in pixels."""
static mp_obj_t sdl_framebuffer_get_height(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(_checked(self_in)->height);
}
MP_DEFINE_CONST_FUN_OBJ_1(sdl_framebuffer_get_height_obj, sdl_framebuffer_get_height);
MP_PROPERTY_GETTER(sdl_framebuffer_height_obj, (mp_obj_t)&sdl_framebuffer_get_height_obj);

//|     color_depth: int
//|     """Bits per pixel, always 16."""
//|
static mp_obj_t sdl_framebuffer_get_color_depth(mp_obj_t self_in) {
    (void)_checked(self_in);
    return MP_OBJ_NEW_SMALL_INT(16);
}
MP_DEFINE_CONST_FUN_OBJ_1(sdl_framebuffer_get_color_depth_obj, sdl_framebuffer_get_color_depth);
MP_PROPERTY_GETTER(sdl_framebuffer_color_depth_obj, (mp_obj_t)&sdl_framebuffer_get_color_depth_obj);

static const mp_rom_map_elem_t sdl_framebuffer_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&sdl_framebuffer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_width), MP_ROM_PTR(&sdl_framebuffer_width_obj) },
    { MP_ROM_QSTR(MP_QSTR_height), MP_ROM_PTR(&sdl_framebuffer_height_obj) },
    { MP_ROM_QSTR(MP_QSTR_color_depth), MP_ROM_PTR(&sdl_framebuffer_color_depth_obj) },
};
static MP_DEFINE_CONST_DICT(sdl_framebuffer_locals_dict, sdl_framebuffer_locals_dict_table);

static void sdl_framebuffer_get_bufinfo(mp_obj_t self_in, mp_buffer_info_t *bufinfo) {
    common_hal_sdl_framebuffer_get_buffer(self_in, bufinfo, 0);
}

static void sdl_framebuffer_swapbuffers(mp_obj_t self_in, uint8_t *dirty_row_bitmap) {
    common_hal_sdl_framebuffer_refresh(MP_OBJ_TO_PTR(self_in));
}

static void sdl_framebuffer_deinit_proto(mp_obj_t self_in) {
    common_hal_sdl_framebuffer_deinit(MP_OBJ_TO_PTR(self_in));
}

static int sdl_framebuffer_get_width_proto(mp_obj_t self_in) {
    return ((sdl_framebuffer_obj_t *)MP_OBJ_TO_PTR(self_in))->width;
}

static int sdl_framebuffer_get_height_proto(mp_obj_t self_in) {
    return ((sdl_framebuffer_obj_t *)MP_OBJ_TO_PTR(self_in))->height;
}

static int sdl_framebuffer_get_row_stride_proto(mp_obj_t self_in) {
    return ((sdl_framebuffer_obj_t *)MP_OBJ_TO_PTR(self_in))->width * sizeof(uint16_t);
}

static mp_float_t sdl_framebuffer_get_brightness_proto(mp_obj_t self_in) {
    (void)self_in;
    return (mp_float_t)desktop_sdl_brightness();
}

static bool sdl_framebuffer_set_brightness_proto(mp_obj_t self_in, mp_float_t brightness) {
    (void)self_in;
    desktop_sdl_set_brightness((float)brightness);
    return true;
}

static const framebuffer_p_t sdl_framebuffer_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_framebuffer)
    .get_bufinfo = sdl_framebuffer_get_bufinfo,
    .get_width = sdl_framebuffer_get_width_proto,
    .get_height = sdl_framebuffer_get_height_proto,
    .get_row_stride = sdl_framebuffer_get_row_stride_proto,
    .swapbuffers = sdl_framebuffer_swapbuffers,
    .deinit = sdl_framebuffer_deinit_proto,
    .get_brightness = sdl_framebuffer_get_brightness_proto,
    .set_brightness = sdl_framebuffer_set_brightness_proto,
};

MP_DEFINE_CONST_OBJ_TYPE(
    sdl_framebuffer_type,
    MP_QSTR_Framebuffer,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    locals_dict, &sdl_framebuffer_locals_dict,
    make_new, sdl_framebuffer_make_new,
    buffer, common_hal_sdl_framebuffer_get_buffer,
    protocol, &sdl_framebuffer_proto
    );
