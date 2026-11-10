// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/countio/Counter.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "bus_trace.h"
#include "desktop_pins.h"
#include "parts.h"

#define COUNTERS 8

static countio_counter_obj_t *_counters[COUNTERS];

void common_hal_countio_counter_construct(countio_counter_obj_t *self,
    const mcu_pin_obj_t *pin, countio_edge_t edge, digitalio_pull_t pull) {
    bool output, level;
    if (!desktop_pin_is_gpio(pin->number) || !desktop_virtual_pin(pin->number, &output, &level)) {
        mp_raise_ValueError(MP_ERROR_TEXT("countio needs the virtual board's pins"));
    }
    size_t slot = 0;
    while (slot < COUNTERS && _counters[slot] != NULL) {
        slot++;
    }
    if (slot == COUNTERS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("All channels in use"));
    }
    common_hal_mcu_pin_claim(pin);
    desktop_pin_backend->configure(pin->number, false, false, pull);
    desktop_virtual_pin(pin->number, &output, &level);
    self->pin = pin;
    self->count = 0;
    self->edge = edge;
    self->level = level;
    _counters[slot] = self;
}

bool common_hal_countio_counter_deinited(countio_counter_obj_t *self) {
    return self->pin == NULL;
}

void common_hal_countio_counter_deinit(countio_counter_obj_t *self) {
    if (common_hal_countio_counter_deinited(self)) {
        return;
    }
    for (size_t i = 0; i < COUNTERS; i++) {
        if (_counters[i] == self) {
            _counters[i] = NULL;
        }
    }
    common_hal_reset_pin(self->pin);
    self->pin = NULL;
}

mp_int_t common_hal_countio_counter_get_count(countio_counter_obj_t *self) {
    return self->count;
}

void common_hal_countio_counter_set_count(countio_counter_obj_t *self, mp_int_t new_count) {
    self->count = new_count;
}

void countio_pin_level(uint8_t pin, bool level) {
    for (size_t i = 0; i < COUNTERS; i++) {
        countio_counter_obj_t *c = _counters[i];
        if (c == NULL || c->pin->number != pin || c->level == level) {
            continue;
        }
        c->level = level;
        if (c->edge == EDGE_RISE_AND_FALL || (c->edge == EDGE_RISE) == level) {
            c->count++;
        }
    }
}

bool countio_add(uint8_t pin, mp_int_t n) {
    bool found = false;
    for (size_t i = 0; i < COUNTERS; i++) {
        countio_counter_obj_t *c = _counters[i];
        if (c != NULL && c->pin->number == pin) {
            c->count += n;
            found = true;
        }
    }
    if (found) {
        char name[24];
        parts_pin_name(pin, name, sizeof(name));
        bus_trace_text("COUNT %s +%ld", name, (long)n);
    }
    return found;
}

void countio_reset(void) {
    for (size_t i = 0; i < COUNTERS; i++) {
        _counters[i] = NULL;
    }
}
