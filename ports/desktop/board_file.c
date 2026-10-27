// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// --board-file FILE: names added to the chosen board, NeoPixel layouts and the board panel's
// parts; --neopixel NAME=SPEC lays NeoPixels out from the command line. A TOML subset, as settings.toml:
//
//     [pins]
//     NEOPIXEL = "GPIO14"     # a pin number, 0-27
//     BUTTON = "SW_A"         # or a name the board has
//     [neopixel]
//     NEOPIXEL = "8x8 serpentine"
//     [gamepad]               # the handheld's buttons on the panel
//     A = "Space, Z"          # and their keys (gamepad.c)
//     [i2c]                   # devices on the virtual I2C bus (i2c_virtual.c)
//     ACCEL = "lis3dh 0x19"
//
// It runs at start-up, before the VM: a new name is a string object outside the heap, so the
// board's table keeps it from one VM to the next.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/obj.h"
#include "py/objstr.h"
#include "py/qstr.h"
#include "shared-bindings/board/__init__.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "gamepad.h"
#include "i2c_virtual.h"
#include "midi_in.h"
#include "spi_virtual.h"
#include "desktop_pins.h"
#include "desktop_port.h"
#include "neopixel_emu.h"
#include "parts.h"

#define MAX_NAMES 128

static mp_map_elem_t *_table;
static size_t _used;

static bool _key_is(mp_obj_t key, const char *name) {
    size_t len;
    const char *str = mp_obj_is_qstr(key) ? (const char *)qstr_data(MP_OBJ_QSTR_VALUE(key), &len)
        : mp_obj_str_get_data(key, &len);
    return len == strlen(name) && memcmp(str, name, len) == 0;
}

static mp_map_elem_t *_find(const char *name) {
    for (size_t i = 0; i < _used; i++) {
        if (_key_is(_table[i].key, name)) {
            return &_table[i];
        }
    }
    return NULL;
}

// "GPIO14", or a name the board has (or got earlier in the file), that is a pin.
static const mcu_pin_obj_t *_pin(const char *target) {
    unsigned n;
    char rest;
    if (sscanf(target, "GPIO%u%c", &n, &rest) == 1) {
        return desktop_pin_obj((uint8_t)(n < 256 ? n : 255));
    }
    mp_map_elem_t *e = _find(target);
    return e != NULL && mp_obj_is_type(e->value, &mcu_pin_type) ? MP_OBJ_TO_PTR(e->value) : NULL;
}

// A key that outlives the VM: a static qstr when there is one, else a string outside the heap.
static mp_obj_t _key(const char *name) {
    qstr q = qstr_find_strn(name, strlen(name));
    if (q != MP_QSTRnull) {
        return MP_OBJ_NEW_QSTR(q);
    }
    mp_obj_str_t *s = malloc(sizeof(mp_obj_str_t));
    char *data = strdup(name);
    if (s == NULL || data == NULL) {
        return MP_OBJ_NULL;
    }
    s->base.type = &mp_type_str;
    s->len = strlen(name);
    s->hash = qstr_compute_hash((const byte *)data, s->len);
    s->data = (const byte *)data;
    return MP_OBJ_FROM_PTR(s);
}

static bool _add_pin(const char *where, const char *name, const char *target) {
    const mcu_pin_obj_t *pin = _pin(target);
    if (pin == NULL) {
        fprintf(stderr, "circuitpython: %s: no pin %s\n", where, target);
        return false;
    }
    mp_map_elem_t *e = _find(name);
    if (e == NULL) {
        if (_used == MAX_NAMES) {
            fprintf(stderr, "circuitpython: %s: more than %d names on the board\n", where, MAX_NAMES);
            return false;
        }
        e = &_table[_used++];
        e->key = _key(name);
        if (e->key == MP_OBJ_NULL) {
            return false;
        }
    }
    e->value = MP_OBJ_FROM_PTR(pin);
    return true;
}

static bool _add_neopixel(const char *where, const char *name, const char *spec) {
    const mcu_pin_obj_t *pin = _pin(name);
    uint16_t cols, rows;
    bool serpentine;
    char order[5];
    if (pin == NULL) {
        fprintf(stderr, "circuitpython: %s: no pin %s\n", where, name);
        return false;
    }
    if (!neopixel_emu_parse(spec, &cols, &rows, &serpentine, order)) {
        fprintf(stderr, "circuitpython: %s: bad NeoPixel layout \"%s\" (\"10\", \"8x8\", \"16x16 serpentine GRBW\")\n", where, spec);
        return false;
    }
    if (!neopixel_emu_add(pin->number, cols, rows, serpentine, order)) {
        fprintf(stderr, "circuitpython: %s: more than %d NeoPixel pins\n", where, NEOPIXEL_EMU_STRIPS);
        return false;
    }
    return true;
}

// A key or a value: spaces around it gone, and its double quotes; NULL when it is empty or a
// quote is not closed.
static char *_unquote(char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    if (*s == '"') {
        if (end - s < 2 || end[-1] != '"') {
            return NULL;
        }
        end[-1] = '\0';
        return s + 1;               // "" is a value: a part with no options
    }
    return *s != '\0' ? s : NULL;
}

// The sections, by their index: names, NeoPixels, the gamepad's keys, I2C and SPI devices, then parts.
#define NONE -1
#define PINS 0
#define NEOPIXEL 1
#define GAMEPAD 2
#define I2C 3
#define SPI 4
#define BOARD 5
#define SHIFTREG 6
#define MIDI 17
static const char *const _sections[] = { "[pins]", "[neopixel]", "[gamepad]", "[i2c]", "[spi]", "[board]", "[shiftreg]", "[button]", "[switch]", "[led]", "[analog]", "[pwm]", "[touch]", "[i2s]", "[mic]", "[encoder]", "[ir]", "[midi]" };
static const part_kind_t _kinds[] = { 0, 0, 0, 0, 0, 0, 0, PART_BUTTON, PART_SWITCH, PART_LED, PART_ANALOG, PART_PWM, PART_TOUCH, PART_SPEAKER, PART_MIC, PART_ENCODER, PART_IR, 0 };

static bool _add_part(const char *where, part_kind_t kind, const char *name, const char *options) {
    const mcu_pin_obj_t *pin = _pin(name);
    const char *why = NULL;
    if (pin == NULL || !desktop_pin_is_gpio(pin->number)) {
        fprintf(stderr, "circuitpython: %s: no pin %s\n", where, name);
        return false;
    }
    if (parts_add(kind, pin->number, name, options, &why) == NULL) {
        fprintf(stderr, "circuitpython: %s: %s: %s\n", where, name, why);
        return false;
    }
    return true;
}

// "MODEL WxH cs=PIN dc=PIN": the pins looked up as [pins] names are.
static bool _add_spi(const char *where, const char *name, const char *spec) {
    char copy[128], rest[128] = "";
    int cs = -1, dc = -1;
    snprintf(copy, sizeof(copy), "%s", spec);
    for (char *word = strtok(copy, " "); word != NULL; word = strtok(NULL, " ")) {
        bool is_cs = strncmp(word, "cs=", 3) == 0, is_dc = strncmp(word, "dc=", 3) == 0;
        if (is_cs || is_dc) {
            const mcu_pin_obj_t *pin = _pin(word + 3);
            if (pin == NULL || !desktop_pin_is_gpio(pin->number)) {
                fprintf(stderr, "circuitpython: %s: no pin %s\n", where, word + 3);
                return false;
            }
            *(is_cs ? &cs : &dc) = pin->number;
        } else {
            strncat(rest, word, sizeof(rest) - strlen(rest) - 2);
            strcat(rest, " ");
        }
    }
    const char *why = NULL;
    if (!spi_virtual_add(name, rest, cs, dc, &why)) {
        fprintf(stderr, "circuitpython: %s: %s\n", where, why);
        return false;
    }
    return true;
}

// NAME = "keyboard [LOW] [channel=N]": the MIDI keyboard; NAME = "cc N [VALUE] [channel=N]": a
// CC slider.
static bool _add_midi(const char *where, const char *name, const char *spec) {
    const char *why = NULL;
    bool ok = false;
    if (strncmp(spec, "keyboard", 8) == 0 && (spec[8] == '\0' || spec[8] == ' ')) {
        ok = midi_keyboard_add(name, spec + 8 + (spec[8] == ' '), &why);
    } else if (strncmp(spec, "cc ", 3) == 0) {
        ok = parts_add_cc(name, spec + 3, &why) != NULL;
    } else {
        why = "a [midi] part is \"keyboard\" or \"cc N\"";
    }
    if (!ok) {
        fprintf(stderr, "circuitpython: %s: %s: %s\n", where, name, why);
    }
    return ok;
}

// DATA = "clock=PIN latch=PIN keys=B,A,START,...": the gamepad's buttons behind a shift register.
static bool _add_shiftreg(const char *where, const char *name, const char *spec) {
    const mcu_pin_obj_t *data = _pin(name);
    const mcu_pin_obj_t *clock = NULL, *latch = NULL;
    const char *keys = "";
    char copy[160];
    snprintf(copy, sizeof(copy), "%s", spec);
    for (char *word = strtok(copy, " "); word != NULL; word = strtok(NULL, " ")) {
        if (strncmp(word, "clock=", 6) == 0) {
            clock = _pin(word + 6);
        } else if (strncmp(word, "latch=", 6) == 0) {
            latch = _pin(word + 6);
        } else if (strncmp(word, "keys=", 5) == 0) {
            keys = word + 5;
        }
    }
    if (data == NULL || clock == NULL || latch == NULL || !desktop_pin_is_gpio(data->number)
        || !desktop_pin_is_gpio(clock->number) || !desktop_pin_is_gpio(latch->number)) {
        fprintf(stderr, "circuitpython: %s: a shift register needs its data, clock= and latch= pins\n", where);
        return false;
    }
    const char *why = NULL;
    if (parts_add_shiftreg(data->number, clock->number, latch->number, name, keys, &why) == NULL) {
        fprintf(stderr, "circuitpython: %s: %s\n", where, why);
        return false;
    }
    return true;
}

static bool _load(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return false;
    }
    char line[256];
    char where[300];
    int section = NONE;
    bool ok = true;
    for (int n = 1; ok && fgets(line, sizeof(line), f) != NULL; n++) {
        snprintf(where, sizeof(where), "%s:%d", path, n);
        // A comment starts at a # outside quotes.
        bool quoted = false;
        for (char *c = line; *c != '\0'; c++) {
            if (*c == '"') {
                quoted = !quoted;
            } else if (*c == '#' && !quoted) {
                *c = '\0';
                break;
            }
        }
        char *s = line;
        while (isspace((unsigned char)*s)) {
            s++;
        }
        char *end = s + strlen(s);
        while (end > s && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }
        if (*s == '\0') {
            continue;
        }
        if (*s == '[') {
            section = NONE;
            for (size_t i = 0; i < sizeof(_sections) / sizeof(_sections[0]); i++) {
                if (strcmp(s, _sections[i]) == 0) {
                    section = (int)i;
                }
            }
            if (section == NONE) {
                fprintf(stderr, "circuitpython: %s: unknown section %s ([pins], [neopixel], [gamepad], [i2c], [spi], [board], [shiftreg], [button], "
                    "[switch], [led], [analog], [pwm], [touch], [i2s], [mic], [encoder], [ir] or [midi])\n", where, s);
                ok = false;
            } else if (section == GAMEPAD) {
                gamepad_show();
            }
            continue;
        }
        char *eq = strchr(s, '=');
        char *key = NULL;
        char *value = NULL;
        if (eq != NULL) {
            *eq = '\0';
            key = _unquote(s);
            value = _unquote(eq + 1);
        }
        if (key == NULL || value == NULL || section == NONE) {
            fprintf(stderr, "circuitpython: %s: expected NAME = \"VALUE\" in a section\n", where);
            ok = false;
        } else if (section == PINS) {
            ok = _add_pin(where, key, value);
        } else if (section == NEOPIXEL) {
            ok = _add_neopixel(where, key, value);
        } else if (section == SHIFTREG) {
            ok = _add_shiftreg(where, key, value);
        } else if (section == BOARD) {
            // platform: sys.platform as the real board names it.
            // machine: os.uname().machine as the real board names it.
            if (strcmp(key, "platform") == 0) {
                ok = desktop_set_platform(value);
            } else if (strcmp(key, "machine") == 0) {
                ok = desktop_set_machine(value);
            } else {
                fprintf(stderr, "circuitpython: %s: unknown [board] setting %s (platform or machine)\n", where, key);
                ok = false;
            }
        } else if (section == SPI) {
            ok = _add_spi(where, key, value);
        } else if (section == MIDI) {
            ok = _add_midi(where, key, value);
        } else if (section == GAMEPAD || section == I2C) {
            const char *why = NULL;
            ok = section == GAMEPAD ? gamepad_set_keys(key, value, &why) : i2c_virtual_add(key, value, &why);
            if (!ok) {
                fprintf(stderr, "circuitpython: %s: %s\n", where, why);
            }
        } else {
            ok = _add_part(where, _kinds[section], key, value);
        }
    }
    fclose(f);
    return ok;
}

bool board_file_apply(void) {
    if (desktop_options.board_file_count == 0 && desktop_options.neopixel_count == 0) {
        return true;
    }
    // The static qstrs, to read the board's names before the VM starts; mp_init does it again.
    qstr_init();
    mp_map_t *map = &board_module_globals.map;
    _table = calloc(MAX_NAMES, sizeof(mp_map_elem_t));
    if (_table == NULL || map->used > MAX_NAMES) {
        return false;
    }
    memcpy(_table, map->table, map->used * sizeof(mp_map_elem_t));
    _used = map->used;
    for (int i = 0; i < desktop_options.board_file_count; i++) {
        if (!_load(desktop_options.board_files[i])) {
            return false;
        }
    }
    for (int i = 0; i < desktop_options.neopixel_count; i++) {
        char spec[128];
        snprintf(spec, sizeof(spec), "%s", desktop_options.neopixel[i]);
        char *eq = strchr(spec, '=');
        if (eq == NULL) {
            fprintf(stderr, "circuitpython: --neopixel %s: expected NAME=LAYOUT, such as NEOPIXEL=8x8\n", spec);
            return false;
        }
        *eq = '\0';
        if (!_add_neopixel("--neopixel", spec, eq + 1)) {
            return false;
        }
    }
    bool all_qstrs = true;
    for (size_t i = 0; i < _used; i++) {
        all_qstrs = all_qstrs && mp_obj_is_qstr(_table[i].key);
    }
    map->table = _table;
    map->used = _used;
    map->alloc = _used;
    map->all_keys_are_qstrs = all_qstrs;
    return true;
}

void board_file_intern_names(void) {
    for (size_t i = 0; i < _used; i++) {
        if (!mp_obj_is_qstr(_table[i].key)) {
            size_t len;
            const char *name = mp_obj_str_get_data(_table[i].key, &len);
            qstr_from_strn(name, len);
        }
    }
}
