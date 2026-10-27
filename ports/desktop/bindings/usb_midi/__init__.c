// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdio.h>

#include "py/obj.h"
#include "py/objtuple.h"
#include "py/runtime.h"

#include "bus_trace.h"
#include "midi_in.h"

//| """USB MIDI on a host
//|
//| The port is no USB device: ``usb_midi.ports`` is a board's, so ``adafruit_midi`` code runs.
//| What comes in is what the board panel's MIDI keyboard and CC sliders play (a board file's
//| ``[midi]`` section, or ``board.attach``), traced as ``MIDI IN 90 3c 64``; what goes out is traced
//| as ``MIDI OUT 90 3c 64``."""

typedef struct {
    mp_obj_base_t base;
} usb_midi_port_obj_t;

extern const mp_obj_type_t usb_midi_portin_type;
extern const mp_obj_type_t usb_midi_portout_type;

//| class PortIn:
//|     def read(self, nbytes: Optional[int] = None) -> Optional[bytes]:
//|         """What has come in, at most nbytes; None: nothing."""
//|     def readinto(self, buf: WriteableBuffer, nbytes: Optional[int] = None) -> Optional[int]:
//|         """What has come in, into buf, at most nbytes: how many; None: nothing."""
static mp_obj_t usb_midi_portin_read(size_t n_args, const mp_obj_t *args) {
    uint8_t buf[MIDI_IN_QUEUE];
    mp_int_t max = n_args > 1 && args[1] != mp_const_none ? mp_obj_get_int(args[1]) : MIDI_IN_QUEUE;
    size_t n = midi_in_read(buf, max < 0 ? 0 : max > MIDI_IN_QUEUE ? MIDI_IN_QUEUE : (size_t)max);
    return n > 0 ? mp_obj_new_bytes(buf, n) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usb_midi_portin_read_obj, 1, 2, usb_midi_portin_read);

static mp_obj_t usb_midi_portin_readinto(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[1], &buf, MP_BUFFER_WRITE);
    size_t max = buf.len;
    if (n_args > 2 && args[2] != mp_const_none) {
        mp_int_t want = mp_obj_get_int(args[2]);
        max = want < 0 ? 0 : (size_t)want < max ? (size_t)want : max;
    }
    size_t n = midi_in_read(buf.buf, max);
    return n > 0 ? MP_OBJ_NEW_SMALL_INT(n) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usb_midi_portin_readinto_obj, 2, 3, usb_midi_portin_readinto);

static const mp_rom_map_elem_t usb_midi_portin_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&usb_midi_portin_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&usb_midi_portin_readinto_obj) },
};
static MP_DEFINE_CONST_DICT(usb_midi_portin_locals_dict, usb_midi_portin_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    usb_midi_portin_type,
    MP_QSTR_PortIn,
    MP_TYPE_FLAG_NONE,
    locals_dict, &usb_midi_portin_locals_dict
    );

//| class PortOut:
//|     def write(self, buf: ReadableBuffer) -> Optional[int]:
//|         """Traced; all of it written."""
static mp_obj_t usb_midi_portout_write(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[1], &buf, MP_BUFFER_READ);
    size_t len = n_args > 2 ? (size_t)mp_obj_get_int(args[2]) : buf.len;
    len = len < buf.len ? len : buf.len;
    char hex[3 * 64 + 1] = "";
    for (size_t k = 0; k < len && k < 64; k++) {
        snprintf(hex + 3 * k, 4, " %02x", ((const uint8_t *)buf.buf)[k]);
    }
    if (len > 0) {
        bus_trace_text("MIDI OUT %s", hex + 1);
    }
    return MP_OBJ_NEW_SMALL_INT(len);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usb_midi_portout_write_obj, 2, 3, usb_midi_portout_write);

static const mp_rom_map_elem_t usb_midi_portout_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&usb_midi_portout_write_obj) },
};
static MP_DEFINE_CONST_DICT(usb_midi_portout_locals_dict, usb_midi_portout_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    usb_midi_portout_type,
    MP_QSTR_PortOut,
    MP_TYPE_FLAG_NONE,
    locals_dict, &usb_midi_portout_locals_dict
    );

static const usb_midi_port_obj_t _in = { { &usb_midi_portin_type } };
static const usb_midi_port_obj_t _out = { { &usb_midi_portout_type } };

static const mp_rom_obj_tuple_t _ports = {
    { &mp_type_tuple }, 2, { MP_ROM_PTR(&_in), MP_ROM_PTR(&_out) },
};

//| ports: Tuple[Union[PortIn, PortOut], ...]
//| def enable() -> None: ...
//| def disable() -> None: ...
//| def set_names(**kwargs) -> None:
//|     """Accepted, as in ``boot.py``."""
static mp_obj_t usb_midi_nothing(void) {
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usb_midi_nothing_obj, usb_midi_nothing);

static mp_obj_t usb_midi_set_names(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    (void)n_args;
    (void)pos_args;
    (void)kw_args;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(usb_midi_set_names_obj, 0, usb_midi_set_names);

static const mp_rom_map_elem_t usb_midi_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_usb_midi) },
    { MP_ROM_QSTR(MP_QSTR_PortIn), MP_ROM_PTR(&usb_midi_portin_type) },
    { MP_ROM_QSTR(MP_QSTR_PortOut), MP_ROM_PTR(&usb_midi_portout_type) },
    { MP_ROM_QSTR(MP_QSTR_ports), MP_ROM_PTR(&_ports) },
    { MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&usb_midi_nothing_obj) },
    { MP_ROM_QSTR(MP_QSTR_disable), MP_ROM_PTR(&usb_midi_nothing_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_names), MP_ROM_PTR(&usb_midi_set_names_obj) },
};
static MP_DEFINE_CONST_DICT(usb_midi_module_globals, usb_midi_module_globals_table);

const mp_obj_module_t usb_midi_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&usb_midi_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_usb_midi, usb_midi_module);
