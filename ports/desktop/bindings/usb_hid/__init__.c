// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>

#include "py/obj.h"
#include "py/objproperty.h"
#include "py/objtuple.h"
#include "py/runtime.h"

#include "bus_trace.h"

//| """USB HID on a host
//|
//| The port is no USB device: the API is a board's, so ``adafruit_hid`` code runs, and every
//| report sent goes to ``--trace-bus`` as ``HID KEYBOARD 02 00 04 ...``. Nothing reaches the
//| computer's keyboard or mouse; no report comes from it."""

#define REPORT_IDS_MAX 4

typedef struct {
    mp_obj_base_t base;
    const char *name;           // in the trace
    uint16_t usage_page;
    uint16_t usage;
    uint8_t num_report_ids;
    uint8_t report_ids[REPORT_IDS_MAX];
    uint8_t in_report_lengths[REPORT_IDS_MAX];
} usb_hid_device_obj_t;

extern const mp_obj_type_t usb_hid_device_type;

static const usb_hid_device_obj_t _keyboard = {
    { &usb_hid_device_type }, "KEYBOARD", 0x01, 0x06, 1, { 1 }, { 8 },
};
static const usb_hid_device_obj_t _mouse = {
    { &usb_hid_device_type }, "MOUSE", 0x01, 0x02, 1, { 2 }, { 4 },
};
static const usb_hid_device_obj_t _consumer_control = {
    { &usb_hid_device_type }, "CONSUMER_CONTROL", 0x0C, 0x01, 1, { 3 }, { 2 },
};

//| class Device:
//|     def __init__(self, *, report_descriptor: ReadableBuffer, usage_page: int, usage: int,
//|         report_ids: Sequence[int], in_report_lengths: Sequence[int],
//|         out_report_lengths: Sequence[int]) -> None:
//|         """A HID device described as on a board; the descriptor is kept to no purpose."""
static mp_obj_t usb_hid_device_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_report_descriptor, ARG_usage_page, ARG_usage, ARG_report_ids, ARG_in_report_lengths, ARG_out_report_lengths };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_report_descriptor, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_usage_page, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_usage, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_report_ids, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_in_report_lengths, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_out_report_lengths, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    usb_hid_device_obj_t *self = mp_obj_malloc(usb_hid_device_obj_t, &usb_hid_device_type);
    self->name = "DEVICE";
    self->usage_page = (uint16_t)args[ARG_usage_page].u_int;
    self->usage = (uint16_t)args[ARG_usage].u_int;
    size_t n_ids, n_lengths;
    mp_obj_t *ids, *lengths;
    mp_obj_get_array(args[ARG_report_ids].u_obj, &n_ids, &ids);
    mp_obj_get_array(args[ARG_in_report_lengths].u_obj, &n_lengths, &lengths);
    if (n_ids < 1 || n_ids > REPORT_IDS_MAX || n_lengths != n_ids) {
        mp_raise_ValueError(MP_ERROR_TEXT("report_ids and in_report_lengths: 1 to 4 of each"));
    }
    self->num_report_ids = (uint8_t)n_ids;
    for (size_t i = 0; i < n_ids; i++) {
        self->report_ids[i] = (uint8_t)mp_obj_get_int(ids[i]);
        self->in_report_lengths[i] = (uint8_t)mp_obj_get_int(lengths[i]);
    }
    return MP_OBJ_FROM_PTR(self);
}

static size_t _report_index(const usb_hid_device_obj_t *self, mp_obj_t report_id) {
    if (report_id == mp_const_none && self->num_report_ids == 1) {
        return 0;
    }
    mp_int_t id = report_id == mp_const_none ? 0 : mp_obj_get_int(report_id);
    for (size_t i = 0; i < self->num_report_ids; i++) {
        if (self->report_ids[i] == id) {
            return i;
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("report_id"));
}

//|     def send_report(self, report: ReadableBuffer, report_id: Optional[int] = None) -> None:
//|         """Traced as the report's bytes in hex."""
static mp_obj_t usb_hid_device_send_report(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_report, ARG_report_id };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_report, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_report_id, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    const usb_hid_device_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_buffer_info_t report;
    mp_get_buffer_raise(args[ARG_report].u_obj, &report, MP_BUFFER_READ);
    size_t i = _report_index(self, args[ARG_report_id].u_obj);
    if (report.len != self->in_report_lengths[i]) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("Buffer incorrect size. Should be %d bytes."), self->in_report_lengths[i]);
    }
    char hex[3 * 64 + 1] = "";
    for (size_t k = 0; k < report.len && k < 64; k++) {
        snprintf(hex + 3 * k, 4, " %02x", ((const uint8_t *)report.buf)[k]);
    }
    bus_trace_text("HID %s %s", self->name, hex + 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(usb_hid_device_send_report_obj, 2, usb_hid_device_send_report);

//|     def get_last_received_report(self, report_id: Optional[int] = None) -> Optional[bytes]:
//|         """None: the host sends no report (a keyboard's LEDs stay off)."""
static mp_obj_t usb_hid_device_get_last_received_report(size_t n_args, const mp_obj_t *args) {
    _report_index(MP_OBJ_TO_PTR(args[0]), n_args > 1 ? args[1] : mp_const_none);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usb_hid_device_get_last_received_report_obj, 1, 2, usb_hid_device_get_last_received_report);

//|     usage_page: int
//|     usage: int
static mp_obj_t usb_hid_device_get_usage_page(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((usb_hid_device_obj_t *)MP_OBJ_TO_PTR(self_in))->usage_page);
}
MP_DEFINE_CONST_FUN_OBJ_1(usb_hid_device_get_usage_page_obj, usb_hid_device_get_usage_page);
MP_PROPERTY_GETTER(usb_hid_device_usage_page_obj, (mp_obj_t)&usb_hid_device_get_usage_page_obj);

static mp_obj_t usb_hid_device_get_usage(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(((usb_hid_device_obj_t *)MP_OBJ_TO_PTR(self_in))->usage);
}
MP_DEFINE_CONST_FUN_OBJ_1(usb_hid_device_get_usage_obj, usb_hid_device_get_usage);
MP_PROPERTY_GETTER(usb_hid_device_usage_obj, (mp_obj_t)&usb_hid_device_get_usage_obj);

static const mp_rom_map_elem_t usb_hid_device_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_send_report), MP_ROM_PTR(&usb_hid_device_send_report_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_last_received_report), MP_ROM_PTR(&usb_hid_device_get_last_received_report_obj) },
    { MP_ROM_QSTR(MP_QSTR_usage_page), MP_ROM_PTR(&usb_hid_device_usage_page_obj) },
    { MP_ROM_QSTR(MP_QSTR_usage), MP_ROM_PTR(&usb_hid_device_usage_obj) },
    { MP_ROM_QSTR(MP_QSTR_KEYBOARD), MP_ROM_PTR(&_keyboard) },
    { MP_ROM_QSTR(MP_QSTR_MOUSE), MP_ROM_PTR(&_mouse) },
    { MP_ROM_QSTR(MP_QSTR_CONSUMER_CONTROL), MP_ROM_PTR(&_consumer_control) },
};
static MP_DEFINE_CONST_DICT(usb_hid_device_locals_dict, usb_hid_device_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    usb_hid_device_type,
    MP_QSTR_Device,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, usb_hid_device_make_new,
    locals_dict, &usb_hid_device_locals_dict
    );

static const mp_rom_obj_tuple_t _devices = {
    { &mp_type_tuple }, 3, { MP_ROM_PTR(&_keyboard), MP_ROM_PTR(&_mouse), MP_ROM_PTR(&_consumer_control) },
};

//| devices: Tuple[Device, ...]
//| def enable(devices: Optional[Sequence[Device]], boot_device: int = 0) -> None:
//|     """Accepted, as in ``boot.py``; the devices stay the default three."""
static mp_obj_t usb_hid_enable(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    (void)n_args;
    (void)pos_args;
    (void)kw_args;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(usb_hid_enable_obj, 1, usb_hid_enable);

//| def disable() -> None: ...
//| def set_interface_name(interface_name: str) -> None: ...
static mp_obj_t usb_hid_disable(void) {
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_hid_disable_obj, usb_hid_disable);

static mp_obj_t usb_hid_set_interface_name(mp_obj_t name) {
    (void)name;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(usb_hid_set_interface_name_obj, usb_hid_set_interface_name);

//| def get_boot_device() -> int:
//|     """0: no host asked for a boot keyboard or mouse."""
static mp_obj_t usb_hid_get_boot_device(void) {
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_hid_get_boot_device_obj, usb_hid_get_boot_device);

static const mp_rom_map_elem_t usb_hid_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_usb_hid) },
    { MP_ROM_QSTR(MP_QSTR_Device), MP_ROM_PTR(&usb_hid_device_type) },
    { MP_ROM_QSTR(MP_QSTR_devices), MP_ROM_PTR(&_devices) },
    { MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&usb_hid_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_disable), MP_ROM_PTR(&usb_hid_disable_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_interface_name), MP_ROM_PTR(&usb_hid_set_interface_name_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_boot_device), MP_ROM_PTR(&usb_hid_get_boot_device_obj) },
};
static MP_DEFINE_CONST_DICT(usb_hid_module_globals, usb_hid_module_globals_table);

const mp_obj_module_t usb_hid_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&usb_hid_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_usb_hid, usb_hid_module);
