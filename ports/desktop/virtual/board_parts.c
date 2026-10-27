// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "board_parts.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "py/objstr.h"
#include "py/runtime.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "i2c_virtual.h"
#include "neopixel_emu.h"
#include "panel.h"

// ---- the catalogue ----

static void _kind_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t print_kind);
static mp_obj_t _kind_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args);

MP_DEFINE_CONST_OBJ_TYPE(
    board_part_kind_type,
    MP_QSTR_PartKind,
    MP_TYPE_FLAG_NONE,
    print, _kind_print,
    call, _kind_call
    );

#define PIN_KIND(q, k, kw, init, fix) \
    static const board_part_kind_t _kind_##q = { \
        .base = { &board_part_kind_type }, .name = MP_QSTR_##q, .category = BOARD_PART_PIN, .kind = k, \
        .keyword = kw, .initial = init, .fixed = fix, \
    }
#define I2C_KIND(q, m) \
    static const board_part_kind_t _kind_##q = { \
        .base = { &board_part_kind_type }, .name = MP_QSTR_##q, .category = BOARD_PART_I2C, .model = m, \
    }

PIN_KIND(Button, PART_BUTTON, MP_QSTR_pressed, "low", NULL);
PIN_KIND(Switch, PART_SWITCH, MP_QSTRnull, NULL, NULL);
PIN_KIND(LED, PART_LED, MP_QSTR_color, "red", NULL);
PIN_KIND(Potentiometer, PART_ANALOG, MP_QSTRnull, NULL, NULL);
PIN_KIND(Lamp, PART_PWM, MP_QSTR_color, "red", NULL);
PIN_KIND(Servo, PART_PWM, MP_QSTRnull, NULL, "servo");
PIN_KIND(Touch, PART_TOUCH, MP_QSTRnull, NULL, NULL);
PIN_KIND(Speaker, PART_SPEAKER, MP_QSTRnull, NULL, NULL);
PIN_KIND(Mic, PART_MIC, MP_QSTR_source, "tone 440", NULL);
PIN_KIND(Encoder, PART_ENCODER, MP_QSTRnull, NULL, NULL);
PIN_KIND(IR, PART_IR, MP_QSTR_code, "255,2,247,8", NULL);
static const board_part_kind_t _kind_NeoPixel = {
    .base = { &board_part_kind_type }, .name = MP_QSTR_NeoPixel, .category = BOARD_PART_NEOPIXEL,
    .keyword = MP_QSTR_layout, .initial = "10",
};
#if CIRCUITPY_VIRTUAL_DEVICES
I2C_KIND(APDS9960, "apds9960");
I2C_KIND(BME280, "bme280");
I2C_KIND(BMP280, "bmp280");
I2C_KIND(CRICKIT, "crickit");
I2C_KIND(DS3231, "ds3231");
I2C_KIND(LC709203F, "lc709203f");
I2C_KIND(LIS3DH, "lis3dh");
I2C_KIND(LIS3MDL, "lis3mdl");
I2C_KIND(LSM6DS33, "lsm6ds33");
I2C_KIND(MCP9808, "mcp9808");
I2C_KIND(SHT31D, "sht31d");
#endif

static const mp_rom_map_elem_t _parts_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_parts) },
    { MP_ROM_QSTR(MP_QSTR_Button), MP_ROM_PTR(&_kind_Button) },
    { MP_ROM_QSTR(MP_QSTR_Switch), MP_ROM_PTR(&_kind_Switch) },
    { MP_ROM_QSTR(MP_QSTR_LED), MP_ROM_PTR(&_kind_LED) },
    { MP_ROM_QSTR(MP_QSTR_Potentiometer), MP_ROM_PTR(&_kind_Potentiometer) },
    { MP_ROM_QSTR(MP_QSTR_Lamp), MP_ROM_PTR(&_kind_Lamp) },
    { MP_ROM_QSTR(MP_QSTR_Servo), MP_ROM_PTR(&_kind_Servo) },
    { MP_ROM_QSTR(MP_QSTR_Touch), MP_ROM_PTR(&_kind_Touch) },
    { MP_ROM_QSTR(MP_QSTR_Speaker), MP_ROM_PTR(&_kind_Speaker) },
    { MP_ROM_QSTR(MP_QSTR_Mic), MP_ROM_PTR(&_kind_Mic) },
    { MP_ROM_QSTR(MP_QSTR_Encoder), MP_ROM_PTR(&_kind_Encoder) },
    { MP_ROM_QSTR(MP_QSTR_IR), MP_ROM_PTR(&_kind_IR) },
    { MP_ROM_QSTR(MP_QSTR_NeoPixel), MP_ROM_PTR(&_kind_NeoPixel) },
    #if CIRCUITPY_VIRTUAL_DEVICES
    { MP_ROM_QSTR(MP_QSTR_APDS9960), MP_ROM_PTR(&_kind_APDS9960) },
    { MP_ROM_QSTR(MP_QSTR_BME280), MP_ROM_PTR(&_kind_BME280) },
    { MP_ROM_QSTR(MP_QSTR_BMP280), MP_ROM_PTR(&_kind_BMP280) },
    { MP_ROM_QSTR(MP_QSTR_CRICKIT), MP_ROM_PTR(&_kind_CRICKIT) },
    { MP_ROM_QSTR(MP_QSTR_DS3231), MP_ROM_PTR(&_kind_DS3231) },
    { MP_ROM_QSTR(MP_QSTR_LC709203F), MP_ROM_PTR(&_kind_LC709203F) },
    { MP_ROM_QSTR(MP_QSTR_LIS3DH), MP_ROM_PTR(&_kind_LIS3DH) },
    { MP_ROM_QSTR(MP_QSTR_LIS3MDL), MP_ROM_PTR(&_kind_LIS3MDL) },
    { MP_ROM_QSTR(MP_QSTR_LSM6DS33), MP_ROM_PTR(&_kind_LSM6DS33) },
    { MP_ROM_QSTR(MP_QSTR_MCP9808), MP_ROM_PTR(&_kind_MCP9808) },
    { MP_ROM_QSTR(MP_QSTR_SHT31D), MP_ROM_PTR(&_kind_SHT31D) },
    #endif
};
static MP_DEFINE_CONST_DICT(_parts_globals, _parts_globals_table);

const mp_obj_module_t board_parts_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&_parts_globals,
};

static const i2c_virtual_model_t *_model(const board_part_kind_t *kind) {
    return kind->category == BOARD_PART_I2C ? i2c_virtual_model(kind->model) : NULL;
}

static void _kind_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t print_kind) {
    const board_part_kind_t *kind = MP_OBJ_TO_PTR(self_in);
    const i2c_virtual_model_t *m = _model(kind);
    if (m != NULL) {
        mp_printf(print, "%q(*, address=0x%02x, name=None):", kind->name, m->address);
        for (int c = 0; c < m->channel_count; c++) {
            mp_printf(print, "%s %s %s", c ? "," : "", m->channels[c].name, m->channels[c].unit);
        }
        return;
    }
    mp_printf(print, "%q(pin, *", kind->name);
    if (kind->keyword != MP_QSTRnull) {
        mp_printf(print, ", %q=\"%s\"", kind->keyword, kind->initial);
    }
    mp_print_str(print, ", name=None)");
}

// ---- a part ----

static void _part_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t print_kind);
static void _part_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest);

typedef struct {
    part_t *part;
    i2c_virtual_device_t *dev;
    int strip;
    bool rgbw;
    bool automatic;     // put there by the automatic attaching
} live_t;

// The live part at the object's place, found anew on each access; false when it is not attached.
static bool _live(const board_part_obj_t *self, live_t *l) {
    memset(l, 0, sizeof(*l));
    if (!self->attached) {
        return false;
    }
    switch (self->kind->category) {
        case BOARD_PART_PIN:
            l->part = parts_find(self->pin, self->kind->kind);
            l->automatic = l->part != NULL && l->part->automatic;
            return l->part != NULL && !l->part->detached;
        case BOARD_PART_I2C:
            l->dev = i2c_virtual_at(self->address);
            l->automatic = l->dev != NULL && l->dev->automatic;
            return l->dev != NULL && l->dev->model == _model(self->kind);
        default:
            l->strip = neopixel_emu_find(self->pin, &l->rgbw, &l->automatic);
            return l->strip >= 0;
    }
}

static void _require(const board_part_obj_t *self, live_t *l) {
    if (!_live(self, l)) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s is not attached"), self->name);
    }
}

// The place, as the panel and the repr name it: the board's name for the pin, or the address.
static void _place(const board_part_obj_t *self, char *buf, size_t len) {
    if (self->kind->category == BOARD_PART_I2C) {
        snprintf(buf, len, "I2C at 0x%02x", (unsigned)self->address);
    } else {
        parts_pin_name(self->pin, buf, len);
    }
}

static void _print_values(const mp_print_t *print, const board_part_obj_t *self, const live_t *l) {
    const part_t *p = l->part;
    if (l->dev != NULL) {
        const i2c_virtual_model_t *m = l->dev->model;
        for (int c = 0; c < m->channel_count; c++) {
            char value[32];
            snprintf(value, sizeof(value), "%.*f", m->channels[c].decimals, l->dev->value[c]);
            mp_printf(print, "%s%s=%s %s", c ? ", " : "", m->channels[c].name, value, m->channels[c].unit);
        }
        return;
    }
    if (p == NULL) {
        uint16_t cols, rows;
        bool serpentine;
        size_t count;
        const uint8_t(*rgbw)[4];
        neopixel_emu_strip((size_t)l->strip, &cols, &rows, &serpentine, &count, &rgbw);
        mp_printf(print, "%u pixels", (unsigned)count);
        return;
    }
    switch (p->kind) {
        case PART_BUTTON:
            mp_print_str(print, p->on ? "pressed" : "released");
            break;
        case PART_SWITCH:
        case PART_LED:
            mp_print_str(print, p->on ? "on" : "off");
            break;
        case PART_TOUCH:
            mp_print_str(print, p->on ? "touched" : "not touched");
            break;
        case PART_ANALOG:
            mp_printf(print, "value=%u", (unsigned)p->value);
            break;
        case PART_MIC:
            mp_printf(print, "level=%u", (unsigned)p->value);
            break;
        case PART_PWM:
            if (!p->pwm_active) {
                mp_print_str(print, "off");
            } else if (p->servo) {
                mp_printf(print, "angle=%d", parts_servo_angle(parts_pulse_us(p->duty, p->frequency)));
            } else {
                mp_printf(print, "duty=%u, frequency=%u Hz", (unsigned)p->duty, (unsigned)p->frequency);
            }
            break;
        case PART_ENCODER:
            mp_printf(print, "position=%d", (int)(p->steps / 4));
            break;
        case PART_SPEAKER:
            mp_print_str(print, p->on ? "playing" : "silent");
            break;
        case PART_IR:
            mp_printf(print, "code=%u,%u,%u,%u", p->ir_code[0], p->ir_code[1], p->ir_code[2], p->ir_code[3]);
            break;
        default:
            break;
    }
}

static void _part_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t print_kind) {
    const board_part_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char place[24];
    live_t l;
    _place(self, place, sizeof(place));
    mp_printf(print, "<%q %s on %s", self->kind->name, self->name, place);
    if (_live(self, &l)) {
        mp_print_str(print, ": ");
        _print_values(print, self, &l);
        mp_print_str(print, l.automatic ? " (automatic)" : "");
    } else {
        mp_print_str(print, ", not attached");
    }
    mp_print_str(print, ">");
}

// A sensor's channel by its name, or -1.
static int _channel(const board_part_obj_t *self, qstr attr) {
    const i2c_virtual_model_t *m = _model(self->kind);
    for (int c = 0; m != NULL && c < m->channel_count; c++) {
        if (strcmp(qstr_str(attr), m->channels[c].name) == 0) {
            return c;
        }
    }
    return -1;
}

// The value attributes of each kind; writable ones set what the panel's control sets.
static bool _is_value(const board_part_obj_t *self, qstr attr, bool *writable) {
    *writable = false;
    if (self->kind->category == BOARD_PART_I2C) {
        *writable = true;
        return _channel(self, attr) >= 0;
    }
    if (self->kind->category == BOARD_PART_NEOPIXEL) {
        return attr == MP_QSTR_colors;
    }
    switch (self->kind->kind) {
        case PART_BUTTON:
            *writable = true;
            return attr == MP_QSTR_pressed;
        case PART_SWITCH:
            *writable = true;
            return attr == MP_QSTR_on;
        case PART_TOUCH:
            *writable = true;
            return attr == MP_QSTR_touched;
        case PART_ANALOG:
            *writable = true;
            return attr == MP_QSTR_value;
        case PART_MIC:
            *writable = true;
            return attr == MP_QSTR_level;
        case PART_LED:
            return attr == MP_QSTR_value;
        case PART_PWM:
            return self->kind->fixed != NULL ? attr == MP_QSTR_angle
                   : attr == MP_QSTR_duty || attr == MP_QSTR_frequency;
        case PART_ENCODER:
            return attr == MP_QSTR_position;
        case PART_SPEAKER:
            return attr == MP_QSTR_playing;
        default:
            return false;
    }
}

static mp_obj_t _colors(const live_t *l) {
    uint16_t cols, rows;
    bool serpentine;
    size_t count;
    const uint8_t(*rgbw)[4];
    neopixel_emu_strip((size_t)l->strip, &cols, &rows, &serpentine, &count, &rgbw);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < count; i++) {
        mp_obj_t items[4];
        for (int k = 0; k < 4; k++) {
            items[k] = MP_OBJ_NEW_SMALL_INT(rgbw[i][k]);
        }
        mp_obj_list_append(list, mp_obj_new_tuple(l->rgbw ? 4 : 3, items));
    }
    return list;
}

static mp_obj_t _load(const board_part_obj_t *self, qstr attr, const live_t *l) {
    if (l->dev != NULL) {
        return mp_obj_new_float(l->dev->value[_channel(self, attr)]);
    }
    const part_t *p = l->part;
    if (p == NULL) {
        return _colors(l);
    }
    switch (attr) {
        case MP_QSTR_pressed:
        case MP_QSTR_on:
        case MP_QSTR_touched:
        case MP_QSTR_playing:
            return mp_obj_new_bool(p->on);
        case MP_QSTR_duty:
            return MP_OBJ_NEW_SMALL_INT(p->pwm_active ? p->duty : 0);
        case MP_QSTR_frequency:
            return mp_obj_new_int_from_uint(p->frequency);
        case MP_QSTR_angle:
            return MP_OBJ_NEW_SMALL_INT(parts_servo_angle(parts_pulse_us(p->duty, p->frequency)));
        case MP_QSTR_position:
            return mp_obj_new_int(p->steps / 4);
        default:
            // An LED's value is whether it is lit, a potentiometer's and a microphone's a number.
            return p->kind == PART_LED ? mp_obj_new_bool(p->on) : MP_OBJ_NEW_SMALL_INT(p->value);
    }
}

static void _store(const board_part_obj_t *self, qstr attr, const live_t *l, mp_obj_t value) {
    if (l->dev != NULL) {
        i2c_virtual_set(l->dev, _channel(self, attr), mp_obj_get_float(value));
        panel_changed();
    } else if (l->part->kind == PART_ANALOG || l->part->kind == PART_MIC) {
        mp_int_t v = mp_obj_get_int(value);
        parts_set(l->part, (uint32_t)(v < 0 ? 0 : v > 65535 ? 65535 : v));
    } else {
        parts_set(l->part, mp_obj_is_true(value));
    }
}

static void _part_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    const board_part_obj_t *self = MP_OBJ_TO_PTR(self_in);
    bool writable;
    live_t l;
    if (dest[0] == MP_OBJ_NULL) {
        if (attr == MP_QSTR_name) {
            dest[0] = mp_obj_new_str(self->name, strlen(self->name));
        } else if (_is_value(self, attr, &writable)) {
            _require(self, &l);
            dest[0] = _load(self, attr, &l);
        } else if ((attr == MP_QSTR_turn && self->kind->kind == PART_ENCODER)
                   || (attr == MP_QSTR_send && self->kind->kind == PART_IR)) {
            dest[1] = MP_OBJ_SENTINEL;  // the method, from the locals
        }
    } else if (dest[1] != MP_OBJ_NULL && _is_value(self, attr, &writable) && writable) {
        _require(self, &l);
        _store(self, attr, &l, dest[1]);
        dest[0] = MP_OBJ_NULL;
    }
}

static mp_obj_t _turn(mp_obj_t self_in, mp_obj_t detents) {
    live_t l;
    _require(MP_OBJ_TO_PTR(self_in), &l);
    parts_turn(l.part, mp_obj_get_int(detents));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(_turn_obj, _turn);

static mp_obj_t _send(size_t n_args, const mp_obj_t *args) {
    live_t l;
    uint8_t code[4];
    _require(MP_OBJ_TO_PTR(args[0]), &l);
    if (n_args > 1 && args[1] != mp_const_none) {
        if (!parts_ir_code(mp_obj_str_get_str(args[1]), code)) {
            mp_raise_ValueError(MP_ERROR_TEXT("bad IR code (four bytes: \"255,2,247,8\")"));
        }
        parts_ir_send(l.part, code);
    } else {
        parts_ir_send(l.part, NULL);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(_send_obj, 1, 2, _send);

static const mp_rom_map_elem_t _part_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_turn), MP_ROM_PTR(&_turn_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&_send_obj) },
};
static MP_DEFINE_CONST_DICT(_part_locals, _part_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    board_part_type,
    MP_QSTR_Part,
    MP_TYPE_FLAG_NONE,
    print, _part_print,
    attr, _part_attr,
    locals_dict, &_part_locals
    );

// A kind called: a part described, not yet on the board.
static mp_obj_t _kind_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    const board_part_kind_t *kind = MP_OBJ_TO_PTR(self_in);
    const i2c_virtual_model_t *m = _model(kind);
    enum { ARG_place, ARG_name, ARG_option };
    mp_arg_t allowed[] = {
        { m != NULL ? MP_QSTR_address : MP_QSTR_pin, m != NULL ? MP_ARG_KW_ONLY | MP_ARG_OBJ : MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_name, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { kind->keyword, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed)];
    mp_map_t kw_args;
    mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
    mp_arg_parse_all(n_args, args, &kw_args, kind->keyword != MP_QSTRnull ? 3 : 2, allowed, vals);

    board_part_obj_t *self = mp_obj_malloc(board_part_obj_t, &board_part_type);
    self->kind = kind;
    self->attached = false;
    size_t limit = sizeof(self->name) - 1;
    if (m != NULL) {
        mp_obj_t address = vals[ARG_place].u_obj;
        self->address = address == mp_const_none ? m->address
            : (uint8_t)mp_arg_validate_int_range(mp_obj_get_int(address), 0x08, 0x77, MP_QSTR_address);
        limit = sizeof(((i2c_virtual_device_t *)NULL)->name) - 1;
        size_t k = 0;
        for (; m->name[k] != '\0' && k < limit; k++) {
            self->name[k] = (char)toupper((unsigned char)m->name[k]);
        }
        self->name[k] = '\0';
    } else {
        const mcu_pin_obj_t *pin = MP_OBJ_TO_PTR(mp_arg_validate_type(vals[ARG_place].u_obj, &mcu_pin_type, MP_QSTR_pin));
        parts_pin_name(pin->number, self->name, sizeof(self->name));
        // The serial and bus pins (TX, SCL, MOSI ...) stand for a bus, not a pin a part can be on.
        if (!desktop_pin_is_gpio(pin->number) || pin->number >= DESKTOP_PIN_UART_TX) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("%s is a bus's pin, not a GPIO"), self->name);
        }
        self->pin = pin->number;
    }
    if (vals[ARG_name].u_obj != mp_const_none && kind->category == BOARD_PART_NEOPIXEL) {
        mp_raise_ValueError(MP_ERROR_TEXT("a NeoPixel strip is named after its pin"));
    }
    if (vals[ARG_name].u_obj != mp_const_none) {
        const char *name = mp_obj_str_get_str(vals[ARG_name].u_obj);
        if (strlen(name) > limit || *name == '\0') {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("a name of 1 to %d characters"), (int)limit);
        }
        strcpy(self->name, name);
    }
    // The options as the board file writes them: "servo", "#ffa000", "8x8 serpentine".
    const char *option = kind->fixed != NULL ? kind->fixed : kind->initial != NULL ? kind->initial : "";
    mp_obj_t value = kind->keyword != MP_QSTRnull ? vals[ARG_option].u_obj : mp_const_none;
    if (mp_obj_is_int(value)) {
        snprintf(self->options, sizeof(self->options), "%d", (int)mp_obj_get_int(value));
    } else {
        snprintf(self->options, sizeof(self->options), "%s", value != mp_const_none ? mp_obj_str_get_str(value) : option);
    }
    return MP_OBJ_FROM_PTR(self);
}

// ---- board.attach, board.detach ----

// An attached pin part at a pin, or NULL.
static part_t *_attached_at(uint8_t pin) {
    for (int i = 0; i < parts_count(); i++) {
        part_t *p = parts_at(i);
        if (p->pin == pin && p->kind != PART_SENSOR) {
            return p;
        }
    }
    return NULL;
}

static mp_obj_t _attach(mp_obj_t part_in) {
    board_part_obj_t *self = MP_OBJ_TO_PTR(mp_arg_validate_type(part_in, &board_part_type, MP_QSTR_part));
    const board_part_kind_t *kind = self->kind;
    const char *why = NULL;
    if (kind->category == BOARD_PART_PIN) {
        if (parts_add(kind->kind, self->pin, self->name, self->options, &why) == NULL) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("%s"), why);
        }
        neopixel_emu_detach(self->pin);
    } else if (kind->category == BOARD_PART_I2C) {
        char spec[32];
        snprintf(spec, sizeof(spec), "%s 0x%02x", kind->model, (unsigned)self->address);
        if (!i2c_virtual_add(self->name, spec, &why)) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("%s"), why);
        }
    } else {
        uint16_t cols, rows;
        bool serpentine;
        char order[5];
        if (!neopixel_emu_parse(self->options, &cols, &rows, &serpentine, order)) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("bad NeoPixel layout \"%s\" (\"10\", \"8x8\", \"16x16 serpentine GRBW\")"), self->options);
        }
        if (!neopixel_emu_add(self->pin, cols, rows, serpentine, order)) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("more than %d NeoPixel pins"), NEOPIXEL_EMU_STRIPS);
        }
        // The strip's data pin is its place: a part there is replaced.
        for (part_t *p; (p = _attached_at(self->pin)) != NULL;) {
            parts_detach(p);
        }
    }
    self->attached = true;
    return part_in;
}
MP_DEFINE_CONST_FUN_OBJ_1(board_attach_obj, _attach);

// A part object, or the name of an attached part (a sensor by its device's name).
static mp_obj_t _detach(mp_obj_t part_in) {
    if (mp_obj_is_str(part_in)) {
        const char *name = mp_obj_str_get_str(part_in);
        part_t *p = parts_by_name(name);
        i2c_virtual_device_t *dev = i2c_virtual_by_name(name);
        if (p != NULL && p->kind != PART_SENSOR && p->kind != PART_SHIFTREG) {
            parts_detach(p);
            return mp_const_none;
        }
        if (dev != NULL && dev->model->attach == NULL) {
            i2c_virtual_detach(dev);
            return mp_const_none;
        }
        for (size_t i = 0; i < neopixel_emu_strips(); i++) {
            char strip_name[24];
            uint8_t pin = neopixel_emu_pin(i);
            parts_pin_name(pin, strip_name, sizeof(strip_name));
            if (strcmp(strip_name, name) == 0 && neopixel_emu_detach(pin)) {
                return mp_const_none;
            }
        }
        mp_raise_ValueError_varg(MP_ERROR_TEXT("no part %s on the board"), name);
    }
    board_part_obj_t *self = MP_OBJ_TO_PTR(mp_arg_validate_type(part_in, &board_part_type, MP_QSTR_part));
    live_t l;
    if (!_live(self, &l)) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%s is not attached"), self->name);
    }
    if (l.part != NULL) {
        parts_detach(l.part);
    } else if (l.dev != NULL) {
        i2c_virtual_detach(l.dev);
    } else {
        neopixel_emu_detach(self->pin);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(board_detach_obj, _detach);

// ---- board.attached ----

// The catalogue's kind of a live part: by its category and part kind, or a sensor by its model.
static const board_part_kind_t *_kind_of(uint8_t category, const part_t *p, const i2c_virtual_model_t *model) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(_parts_globals_table); i++) {
        if (!mp_obj_is_type(_parts_globals_table[i].value, &board_part_kind_type)) {
            continue;
        }
        const board_part_kind_t *kind = MP_OBJ_TO_PTR(_parts_globals_table[i].value);
        if (kind->category != category) {
            continue;
        }
        if ((category == BOARD_PART_NEOPIXEL)
            || (category == BOARD_PART_I2C && _model(kind) == model)
            || (category == BOARD_PART_PIN && kind->kind == p->kind && (kind->fixed != NULL) == (p->kind == PART_PWM && p->servo))) {
            return kind;
        }
    }
    return NULL;
}

static void _add(mp_obj_t dict, const board_part_kind_t *kind, const char *name, uint8_t pin, uint8_t address, const char *options) {
    if (kind == NULL) {
        return;     // a display, a shift register: no kind in the catalogue
    }
    board_part_obj_t *part = mp_obj_malloc(board_part_obj_t, &board_part_type);
    part->kind = kind;
    part->pin = pin;
    part->address = address;
    part->attached = true;
    snprintf(part->options, sizeof(part->options), "%s", options);
    snprintf(part->name, sizeof(part->name), "%s", name);
    mp_obj_dict_store(dict, mp_obj_new_str(name, strlen(name)), MP_OBJ_FROM_PTR(part));
}

// A live pin part's options, as the board file writes them.
static void _options_of(const part_t *p, char *buf, size_t len) {
    switch (p->kind) {
        case PART_BUTTON:
            snprintf(buf, len, "%s", p->active_high ? "high" : "low");
            break;
        case PART_LED:
            snprintf(buf, len, "#%06lx", (unsigned long)p->colour);
            break;
        case PART_PWM:
            snprintf(buf, len, p->servo ? "servo" : "#%06lx", (unsigned long)p->colour);
            break;
        case PART_MIC:
            if (p->source == MIC_TONE) {
                snprintf(buf, len, "tone %lu", (unsigned long)p->tone_hz);
            } else {
                snprintf(buf, len, "%s", p->source == MIC_NOISE ? "noise" : "mic");
            }
            break;
        case PART_IR:
            snprintf(buf, len, "%u,%u,%u,%u", p->ir_code[0], p->ir_code[1], p->ir_code[2], p->ir_code[3]);
            break;
        default:
            buf[0] = '\0';
    }
}

// board.attached, made anew on each read: name -> part object of every part on the board.
static mp_obj_t _getattr(mp_obj_t attr) {
    if (mp_obj_str_get_qstr(attr) != MP_QSTR_attached) {
        // Not found, as the module's own lookup says it: an AttributeError where one is due, and
        // nothing raised for the lookups that only ask (import's __init__, Tab completion).
        return MP_OBJ_NULL;
    }
    mp_obj_t dict = mp_obj_new_dict(0);
    for (int i = 0; i < parts_count(); i++) {
        part_t *p = parts_at(i);
        if (p->kind != PART_SENSOR) {
            char options[64];
            _options_of(p, options, sizeof(options));
            _add(dict, _kind_of(BOARD_PART_PIN, p, NULL), p->name, p->pin, 0, options);
        }
    }
    i2c_virtual_device_t *dev;
    for (int i = 0; (dev = i2c_virtual_device(i)) != NULL; i++) {
        if (dev->present) {
            _add(dict, _kind_of(BOARD_PART_I2C, NULL, dev->model), dev->name, 0, dev->address, "");
        }
    }
    for (size_t i = 0; i < neopixel_emu_strips(); i++) {
        bool rgbw, automatic;
        uint8_t pin = neopixel_emu_pin(i);
        char name[24];
        if (neopixel_emu_find(pin, &rgbw, &automatic) >= 0) {
            uint16_t cols, rows;
            bool serpentine;
            size_t count;
            const uint8_t(*colors)[4];
            char layout[48];
            neopixel_emu_strip(i, &cols, &rows, &serpentine, &count, &colors);
            snprintf(layout, sizeof(layout), "%ux%u%s %s", (unsigned)cols, (unsigned)rows,
                serpentine ? " serpentine" : "", neopixel_emu_order(i));
            parts_pin_name(pin, name, sizeof(name));
            _add(dict, _kind_of(BOARD_PART_NEOPIXEL, NULL, NULL), name, pin, 0, layout);
        }
    }
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(board_getattr_obj, _getattr);
