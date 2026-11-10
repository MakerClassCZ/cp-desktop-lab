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
#if CIRCUITPY_EPAPERDISPLAY
#include "shared-bindings/epaperdisplay/EPaperDisplay.h"
#endif
#include "shared-bindings/microcontroller/Pin.h"
#if CIRCUITPY_DISPLAYIO
#include "shared-module/displayio/__init__.h"
#endif

#include "after_run.h"
#include "desktop_pins.h"
#include "desktop_port.h"
#include "fault.h"
#include "playbook.h"

static bool _vm_was_up;
static uint64_t _claimed_at_start;
static size_t _outside_at_end;
typedef struct {
    char status[8];             // "" when the run reported none
    char detail[200];
} result_t;
static result_t _reported;     // by the VM running now
static struct {
    unsigned runs;
    uint64_t leaked;
    size_t outside;
    long outside_change;
    result_t result;
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
    #if CIRCUITPY_EPAPERDISPLAY
    // An e-paper display's busy pin is its own, inside the display.
    for (size_t i = 0; i < CIRCUITPY_DISPLAY_LIMIT; i++) {
        if (displays[i].display_base.type == &epaperdisplay_epaperdisplay_type &&
            displays[i].epaper_display.busy.base.type == &digitalio_digitalinout_type) {
            pins |= _bit(displays[i].epaper_display.busy.pin);
        }
    }
    #endif
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
    _last.result = _reported;
    const char *status, *detail;
    if (playbook_run_ended(&status, &detail)
        && (strcmp(status, "fail") == 0 ? strcmp(_reported.status, "fail") != 0 : _reported.status[0] == '\0')) {
        // A playbook's fail outranks what the program reported but its fail; its pass is the
        // result only when the program reported none.
        snprintf(_last.result.status, sizeof(_last.result.status), "%s", status);
        snprintf(_last.result.detail, sizeof(_last.result.detail), "%s", detail);
    }
    fault_run_ended();
    _reported = (result_t) {0};
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

void after_run_claimed_json(api_buf_t *b) {
    uint64_t pins = desktop_pins_claimed();
    api_buf_add(b, "[", 1);
    for (uint8_t n = 0, first = 1; n < 64; n++) {
        if (pins & (1ull << n)) {
            char name[32];
            desktop_pin_name(n, name, sizeof(name));
            api_buf_add(b, ", ", first ? 0 : 2);
            api_buf_json_string(b, name, strlen(name));
            first = 0;
        }
    }
    api_buf_add(b, "]", 1);
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

void after_run_report(const char *status, const char *detail) {
    snprintf(_reported.status, sizeof(_reported.status), "%s", status);
    snprintf(_reported.detail, sizeof(_reported.detail), "%s", detail);
}

void after_run_json(api_buf_t *b) {
    api_buf_printf(b, "{\"vm_running\": %s, \"runs\": %u, \"result\": ", gc_alloc_possible() ? "true" : "false",
        _last.runs);
    if (_last.result.status[0] == '\0') {
        api_buf_add(b, "null", 4);
    } else {
        api_buf_add(b, "{\"status\": ", 11);
        api_buf_json_string(b, _last.result.status, strlen(_last.result.status));
        api_buf_add(b, ", \"detail\": ", 12);
        api_buf_json_string(b, _last.result.detail, strlen(_last.result.detail));
        api_buf_add(b, "}", 1);
    }
    api_buf_add(b, ", \"faults_fired\": ", 18);
    fault_last_run_json(b);
    api_buf_add(b, ", \"leaked_pins\": [", 18);
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
