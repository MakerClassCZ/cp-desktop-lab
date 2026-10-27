// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "parts.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "py/obj.h"
#include "py/objstr.h"
#include "py/qstr.h"
#include "shared-bindings/board/__init__.h"

#include "common-hal/audioio/AudioOut.h"
#include "common-hal/pulseio/PulseIn.h"

#include "bus_trace.h"
#include "gamepad.h"
#include "i2c_virtual.h"
#include "desktop_pins.h"
#include "desktop_port.h"
#include "panel.h"

static part_t _parts[PARTS_MAX];
static int _count;

static const struct {
    const char *name;
    uint32_t rgb;
} _colours[] = {
    { "red", 0xff2020 }, { "green", 0x20e020 }, { "blue", 0x3060ff }, { "yellow", 0xffe020 },
    { "orange", 0xff9000 }, { "white", 0xffffff }, { "amber", 0xffb000 },
};

static bool _colour(const char *s, uint32_t *rgb) {
    unsigned v;
    char rest;
    if (*s == '\0') {
        *rgb = 0xff2020;
        return true;
    }
    if (sscanf(s, "#%6x%c", &v, &rest) == 1 && strlen(s) == 7) {
        *rgb = v;
        return true;
    }
    for (size_t i = 0; i < sizeof(_colours) / sizeof(_colours[0]); i++) {
        if (strcasecmp(s, _colours[i].name) == 0) {
            *rgb = _colours[i].rgb;
            return true;
        }
    }
    return false;
}

static bool _options(part_t *p, const char *options, const char **why) {
    p->active_high = false;
    p->colour = 0xff2020;
    p->servo = false;
    switch (p->kind) {
        case PART_BUTTON:
            if (strcmp(options, "high") == 0) {
                p->active_high = true;
            } else if (*options != '\0' && strcmp(options, "low") != 0) {
                *why = "bad button (\"low\" or \"high\")";
                return false;
            }
            return true;
        case PART_LED:
            if (!_colour(options, &p->colour)) {
                *why = "bad colour (red, green, blue, yellow, orange, white, amber or #rrggbb)";
                return false;
            }
            return true;
        case PART_MIC: {
            unsigned hz;
            char rest;
            p->source = MIC_TONE;
            p->tone_hz = 440;
            if (strcmp(options, "noise") == 0) {
                p->source = MIC_NOISE;
            } else if (strcmp(options, "mic") == 0) {
                p->source = MIC_HOST;
            } else if (sscanf(options, "tone %u%c", &hz, &rest) == 1 && hz > 0 && hz < 100000) {
                p->tone_hz = hz;
            } else if (*options != '\0' && strcmp(options, "tone") != 0) {
                *why = "bad microphone (\"tone 440\", \"noise\" or \"mic\")";
                return false;
            }
            return true;
        }
        case PART_IR:
            // The Adafruit mini remote's 1 unless the board file names another code.
            if (!parts_ir_code(*options != '\0' ? options : "255,2,247,8", p->ir_code)) {
                *why = "bad IR code (four bytes: \"255,2,247,8\")";
                return false;
            }
            return true;
        case PART_PWM:
            if (strcmp(options, "servo") == 0) {
                p->servo = true;
            } else if (!_colour(options, &p->colour)) {
                *why = "bad PWM part (\"servo\", or a lamp colour)";
                return false;
            }
            return true;
        default:
            if (*options != '\0') {
                *why = "this part takes no options";
                return false;
            }
            return true;
    }
}

// What a button or switch makes its pin read.
static void _drive(part_t *p) {
    bool level = p->kind == PART_BUTTON ? (p->on == p->active_high) : p->on;
    desktop_pin_drive(p->pin, level);
}

part_t *parts_find(uint8_t pin, part_kind_t kind) {
    for (int i = 0; i < _count; i++) {
        if (_parts[i].pin == pin && _parts[i].kind == kind) {
            return &_parts[i];
        }
    }
    return NULL;
}

part_t *parts_by_name(const char *name) {
    for (int i = 0; i < _count; i++) {
        if (strcasecmp(_parts[i].name, name) == 0) {
            return &_parts[i];
        }
    }
    return NULL;
}

part_t *parts_at(int i) {
    return i >= 0 && i < _count ? &_parts[i] : NULL;
}

int parts_count(void) {
    return _count;
}

part_t *parts_add(part_kind_t kind, uint8_t pin, const char *name, const char *options, const char **why) {
    part_t *p = parts_find(pin, kind);
    bool fresh = p == NULL;
    if (fresh) {
        if (_count == PARTS_MAX) {
            *why = "too many parts";
            return NULL;
        }
        p = &_parts[_count++];
        memset(p, 0, sizeof(*p));
    }
    p->kind = kind;
    p->pin = pin;
    snprintf(p->name, sizeof(p->name), "%s", name);
    if (!_options(p, options, why)) {
        if (fresh) {
            _count--;
        }
        return NULL;
    }
    if (kind == PART_ANALOG || kind == PART_MIC) {
        p->value = 32768;
    }
    if (kind == PART_BUTTON || kind == PART_SWITCH) {
        _drive(p);
    }
    panel_changed();
    return p;
}

void parts_pin_name(uint8_t pin, char *name, size_t len) {
    const mcu_pin_obj_t *obj = desktop_pin_obj(pin);
    const mp_map_t *map = &board_module_globals.map;
    for (size_t i = 0; obj != NULL && i < map->used; i++) {
        if (map->table[i].value == MP_OBJ_FROM_PTR(obj)) {
            size_t n;
            mp_obj_t key = map->table[i].key;
            const char *s = mp_obj_is_qstr(key) ? (const char *)qstr_data(MP_OBJ_QSTR_VALUE(key), &n)
                : mp_obj_str_get_data(key, &n);
            snprintf(name, len, "%.*s", (int)n, s);
            return;
        }
    }
    snprintf(name, len, "GPIO%u", (unsigned)pin);
}

part_t *parts_for_pin(uint8_t pin, part_kind_t kind) {
    part_t *p = parts_find(pin, kind);
    if (p == NULL) {
        char name[16];
        const char *why;
        parts_pin_name(pin, name, sizeof(name));
        p = parts_add(kind, pin, name, "", &why);
    }
    return p;
}

void parts_press(part_t *p, bool down) {
    switch (p->kind) {
        case PART_BUTTON:
            if (p->on != down) {
                p->on = down;
                _drive(p);
                bus_trace_text("BUTTON %s %s", p->name, down ? "pressed" : "released");
            }
            break;
        case PART_TOUCH:
            if (p->on != down) {
                p->on = down;
                bus_trace_text("TOUCH %s %s", p->name, down ? "on" : "off");
            }
            break;
        case PART_SWITCH:
            if (down) {
                parts_set(p, !p->on);
            }
            break;
        default:
            return;
    }
    panel_changed();
}

void parts_set(part_t *p, uint32_t value) {
    switch (p->kind) {
        case PART_SWITCH:
            p->on = value != 0;
            _drive(p);
            bus_trace_text("SWITCH %s %s", p->name, p->on ? "on" : "off");
            break;
        case PART_BUTTON:
        case PART_TOUCH:
            parts_press(p, value != 0);
            return;
        case PART_ANALOG: {
            p->value = (uint16_t)(value > 65535 ? 65535 : value);
            unsigned mv = (unsigned)((uint32_t)p->value * 3300 / 65535);
            bus_trace_text("PANEL %s = %u (%u.%03u V)", p->name, (unsigned)p->value, mv / 1000, mv % 1000);
            break;
        }
        case PART_MIC:
            p->value = (uint16_t)(value > 65535 ? 65535 : value);
            bus_trace_text("PANEL %s = %u (%u%%)", p->name, (unsigned)p->value, (unsigned)((uint32_t)p->value * 100 / 65535));
            break;
        default:
            return;
    }
    panel_changed();
}

part_t *parts_add_sensor(void *device, int channel) {
    i2c_virtual_device_t *dev = device;
    if (_count == PARTS_MAX) {
        return NULL;
    }
    part_t *p = &_parts[_count++];
    memset(p, 0, sizeof(*p));
    p->kind = PART_SENSOR;
    p->pin = 0xff;
    p->device = device;
    p->channel = (uint8_t)channel;
    snprintf(p->name, sizeof(p->name), "%s.%s", dev->name, dev->model->channels[channel].name);
    panel_changed();
    return p;
}

void parts_set_sensor(part_t *p, double value) {
    if (p->kind == PART_SENSOR) {
        i2c_virtual_set(p->device, p->channel, value);
        panel_changed();
    }
}

void parts_turn(part_t *p, int detents) {
    if (p->kind != PART_ENCODER || detents == 0) {
        return;
    }
    p->steps += detents * 4;
    bus_trace_text("ENCODER %s %ld", p->name, (long)(p->steps / 4));
    panel_changed();
}

part_t *parts_add_shiftreg(uint8_t data, uint8_t clock, uint8_t latch, const char *name, const char *keys, const char **why) {
    static char message[64];
    if (_count == PARTS_MAX) {
        *why = "too many parts";
        return NULL;
    }
    part_t *p = &_parts[_count];
    memset(p, 0, sizeof(*p));
    p->kind = PART_SHIFTREG;
    p->pin = data;
    p->clock_pin = clock;
    p->latch_pin = latch;
    snprintf(p->name, sizeof(p->name), "%s", name);
    char list[128];
    snprintf(list, sizeof(list), "%s", keys);
    for (char *key = strtok(list, ","); key != NULL && p->key_count < 16; key = strtok(NULL, ",")) {
        int button = gamepad_button(key);
        if (button < 0) {
            snprintf(message, sizeof(message), "no gamepad button %s", key);
            *why = message;
            return NULL;
        }
        p->keys[p->key_count++] = (int8_t)button;
    }
    _count++;
    return p;
}

bool parts_pin_level(uint8_t pin, bool *level) {
    part_t *p = parts_find(pin, PART_SHIFTREG);
    if (p == NULL) {
        return false;
    }
    *level = p->bit < p->key_count && ((p->latched >> p->bit) & 1);
    return true;
}

bool parts_ir_code(const char *text, uint8_t code[4]) {
    char *end;
    for (int i = 0; i < 4; i++) {
        long value = strtol(text, &end, 0);
        if (end == text || value < 0 || value > 255 || *end != (i < 3 ? ',' : '\0')) {
            return false;
        }
        code[i] = (uint8_t)value;
        text = end + 1;
    }
    return true;
}

void parts_ir_send(part_t *p, const uint8_t code[4]) {
    if (p->kind != PART_IR) {
        return;
    }
    if (code != NULL) {
        memcpy(p->ir_code, code, 4);
    }
    // NEC: a 9 ms mark and 4.5 ms space, a 560 us mark before each bit's space (1690 us a one,
    // 560 us a zero), a closing mark.
    uint16_t pulses[67];
    int n = 0;
    pulses[n++] = 9000;
    pulses[n++] = 4500;
    for (int i = 0; i < 32; i++) {
        pulses[n++] = 560;
        pulses[n++] = (p->ir_code[i / 8] >> (7 - i % 8)) & 1 ? 1690 : 560;
    }
    pulses[n++] = 560;
    if (p->pulsein != NULL) {
        desktop_pulsein_push(p->pulsein, pulses, n);
    }
    p->ir_sent = true;
    bus_trace_text("IR %s %u,%u,%u,%u", p->name, p->ir_code[0], p->ir_code[1], p->ir_code[2], p->ir_code[3]);
    panel_changed();
}

// A shift register sees its latch and clock: an edge of the latch loads the buttons and starts
// over, a rising clock shifts the next one out.
static void _shift(uint8_t pin) {
    bool output, level;
    if (!desktop_virtual_pin(pin, &output, &level)) {
        return;
    }
    for (int i = 0; i < _count; i++) {
        part_t *p = &_parts[i];
        if (p->kind != PART_SHIFTREG) {
            continue;
        }
        if (pin == p->latch_pin && level != p->latch_level) {
            p->latch_level = level;
            p->latched = 0;
            for (int k = 0; k < p->key_count; k++) {
                p->latched |= (uint16_t)(desktop_button_held(p->keys[k]) ? 1u << k : 0);
            }
            p->bit = 0;
        } else if (pin == p->clock_pin && level != p->clock_level) {
            p->clock_level = level;
            if (level) {
                p->bit++;
            }
        }
    }
}

void parts_pin_changed(uint8_t pin) {
    _shift(pin);
    part_t *p = parts_find(pin, PART_LED);
    bool output, level;
    if (p == NULL || !desktop_virtual_pin(pin, &output, &level)) {
        return;
    }
    bool on = output && level;
    if (on != p->on) {
        p->on = on;
        bus_trace_text("LED %s %s", p->name, on ? "on" : "off");
        panel_changed();
    }
}

uint32_t parts_pulse_us(uint16_t duty, uint32_t frequency) {
    if (frequency == 0) {
        return 0;
    }
    uint64_t period = (uint64_t)65535 * frequency;
    return (uint32_t)(((uint64_t)duty * 1000000 + period / 2) / period);
}

int parts_servo_angle(uint32_t pulse_us) {
    int angle = ((int)pulse_us - 750) * 180 / 1500;
    return angle < 0 ? 0 : angle > 180 ? 180 : angle;
}

void parts_pwm(part_t *p, uint16_t duty, uint32_t frequency, bool active) {
    if (p->pwm_active == active && p->duty == duty && p->frequency == frequency) {
        return;
    }
    p->duty = duty;
    p->frequency = frequency;
    p->pwm_active = active;
    if (!active) {
        bus_trace_text("PWM %s off", p->name);
    } else {
        unsigned tenths = (unsigned)(((uint32_t)duty * 1000 + 32767) / 65535);
        uint32_t pulse = parts_pulse_us(duty, frequency);
        if (p->servo) {
            bus_trace_text("PWM %s duty %u (%u.%u%%) %lu Hz %lu us %d deg", p->name, (unsigned)duty, tenths / 10,
                tenths % 10, (unsigned long)frequency, (unsigned long)pulse, parts_servo_angle(pulse));
        } else {
            bus_trace_text("PWM %s duty %u (%u.%u%%) %lu Hz %lu us", p->name, (unsigned)duty, tenths / 10,
                tenths % 10, (unsigned long)frequency, (unsigned long)pulse);
        }
    }
    panel_changed();
}

void parts_speaker(part_t *p, bool playing, uint32_t sample_rate, uint8_t bits, uint8_t channels) {
    if (playing) {
        p->sample_rate = sample_rate;
        p->bits = bits;
        p->channels = channels;
        bus_trace_text("%s %s play %lu Hz %u-bit %s", p->label ? p->label : "I2S", p->name, (unsigned long)sample_rate,
            (unsigned)bits, channels == 1 ? "mono" : "stereo");
    } else if (p->on) {
        bus_trace_text("%s %s stop", p->label ? p->label : "I2S", p->name);
    }
    p->on = playing;
    p->peak = 0;
    panel_changed();
}

void parts_poll(void) {
    for (int i = 0; i < _count; i++) {
        part_t *p = &_parts[i];
        if (p->kind != PART_SPEAKER || p->out == NULL) {
            continue;
        }
        audioio_audioout_obj_t *out = p->out;
        if (p->on && !out->playing) {
            parts_speaker(p, false, 0, 0, 0);
        } else if (p->on && out->peak != p->peak) {
            p->peak = out->peak;
            panel_changed();
        }
    }
}

void parts_reset(void) {
    for (int i = 0; i < _count; i++) {
        part_t *p = &_parts[i];
        if (p->kind == PART_SPEAKER) {
            p->out = NULL;
            p->on = false;
            p->peak = 0;
        } else if (p->kind == PART_PWM) {
            p->pwm_active = false;
        } else if (p->kind == PART_IR) {
            p->pulsein = NULL;
        }
    }
    panel_changed();
}
