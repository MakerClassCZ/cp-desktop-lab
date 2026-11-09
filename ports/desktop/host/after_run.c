// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>

#include "py/gc.h"
#include "py/obj.h"
#include "shared-bindings/digitalio/DigitalInOut.h"
#include "shared-bindings/microcontroller/Pin.h"
#if CIRCUITPY_DISPLAYIO
#include "shared-module/displayio/__init__.h"
#endif

#include "after_run.h"
#include "desktop_pins.h"
#include "desktop_port.h"

static bool _vm_was_up;
static uint64_t _claimed_at_start;
static size_t _outside_at_end;
static struct {
    unsigned runs;
    uint64_t leaked;
    size_t outside;
    long outside_change;
} _last;

#if CIRCUITPY_DISPLAYIO
static uint64_t _bit(const mcu_pin_obj_t *pin) {
    return pin != NULL && pin->number < 64 ? 1ull << pin->number : 0;
}

// A DigitalInOut a display bus made for itself lives outside the heap and outlives the VM.
static uint64_t _own_dio(mp_obj_t dio, bool own) {
    if (!own || dio == mp_const_none || dio == MP_OBJ_NULL || !mp_obj_is_type(dio, &digitalio_digitalinout_type)) {
        return 0;
    }
    return _bit(((digitalio_digitalinout_obj_t *)MP_OBJ_TO_PTR(dio))->pin);
}

// The pins of the displays kept after the VM, as the supervisor keeps them for the terminal.
static uint64_t _display_pins(void) {
    uint64_t pins = 0;
    for (size_t i = 0; i < CIRCUITPY_DISPLAY_LIMIT; i++) {
        primary_display_bus_t *bus = &display_buses[i];
        const mp_obj_type_t *type = bus->bus_base.type;
        if (type == NULL || type == &mp_type_NoneType) {
            continue;
        }
        #if CIRCUITPY_FOURWIRE
        if (type == &fourwire_fourwire_type) {
            // reset_displays() has moved a bus of the heap inline; another one is the board's.
            fourwire_fourwire_obj_t *fw = &bus->fourwire_bus;
            pins |= _bit(fw->bus->clock) | _bit(fw->bus->mosi) | _bit(fw->bus->miso);
            pins |= _own_dio(fw->command, fw->own_command) | _own_dio(fw->chip_select, fw->own_chip_select) |
                _own_dio(fw->reset, fw->own_reset);
        }
        #endif
        #if CIRCUITPY_I2CDISPLAYBUS
        if (type == &i2cdisplaybus_i2cdisplaybus_type) {
            i2cdisplaybus_i2cdisplaybus_obj_t *i2c = &bus->i2cdisplay_bus;
            pins |= _bit(i2c->bus->scl) | _bit(i2c->bus->sda) | _bit(i2c->reset.pin);
        }
        #endif
        #if CIRCUITPY_RGBMATRIX
        if (type == &rgbmatrix_RGBMatrix_type) {
            rgbmatrix_rgbmatrix_obj_t *m = &bus->rgbmatrix;
            for (size_t j = 0; j < m->rgb_count; j++) {
                pins |= 1ull << m->rgb_pins[j];
            }
            for (size_t j = 0; j < m->addr_count; j++) {
                pins |= 1ull << m->addr_pins[j];
            }
            pins |= 1ull << m->clock_pin | 1ull << m->latch_pin | 1ull << m->oe_pin;
        }
        #endif
        #if CIRCUITPY_PICODVI
        if (type == &picodvi_framebuffer_type) {
            pins |= bus->picodvi.pins;
        }
        #endif
    }
    return pins;
}
#endif

static void _ended(void) {
    uint64_t leaked = desktop_pins_claimed() & ~_claimed_at_start;
    #if CIRCUITPY_DISPLAYIO
    leaked &= ~_display_pins();
    #endif
    size_t outside = desktop_port_ram_used();
    _last.runs++;
    _last.leaked = leaked;
    _last.outside = outside;
    _last.outside_change = _last.runs == 1 ? 0 : (long)outside - (long)_outside_at_end;
    _outside_at_end = outside;
    if (leaked != 0) {
        fprintf(stderr, "circuitpython: still claimed after the run:");
        for (uint8_t n = 0; n < 64; n++) {
            if (leaked & (1ull << n)) {
                char name[32];
                desktop_pin_name(n, name, sizeof(name));
                fprintf(stderr, " %s", name);
            }
        }
        fprintf(stderr, " (no object released it; a board keeps it claimed too)\n");
    }
}

void after_run_task(void) {
    bool up = gc_alloc_possible();
    if (up && !_vm_was_up) {
        _claimed_at_start = desktop_pins_claimed();
    } else if (!up && _vm_was_up) {
        _ended();
    }
    _vm_was_up = up;
}

void after_run_json(api_buf_t *b) {
    api_buf_printf(b, "{\"vm_running\": %s, \"runs\": %u, \"leaked_pins\": [", gc_alloc_possible() ? "true" : "false",
        _last.runs);
    bool first = true;
    for (uint8_t n = 0; n < 64; n++) {
        if (_last.leaked & (1ull << n)) {
            char name[32];
            desktop_pin_name(n, name, sizeof(name));
            api_buf_add(b, first ? "" : ", ", first ? 0 : 2);
            api_buf_json_string(b, name, strlen(name));
            first = false;
        }
    }
    api_buf_printf(b, "], \"outside_heap\": %lu, \"outside_heap_change\": %ld}",
        (unsigned long)_last.outside, _last.outside_change);
}
