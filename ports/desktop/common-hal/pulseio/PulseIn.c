// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/gc.h"
#include "py/runtime.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/pulseio/PulseIn.h"

#include "desktop_pins.h"
#include "parts.h"

void common_hal_pulseio_pulsein_construct(pulseio_pulsein_obj_t *self,
    const mcu_pin_obj_t *pin, uint16_t maxlen, bool idle_state) {
    bool output, level;
    if (!desktop_pin_is_gpio(pin->number) || !desktop_virtual_pin(pin->number, &output, &level)) {
        mp_raise_ValueError(MP_ERROR_TEXT("pulseio needs the virtual board's pins"));
    }
    part_t *part = parts_for_pin(pin->number, PART_IR);
    if (part == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("too many parts"));
    }
    self->buffer = m_malloc(maxlen * sizeof(uint16_t));
    common_hal_mcu_pin_claim(pin);
    self->pin = pin;
    self->part = part;
    self->maxlen = maxlen;
    self->start = 0;
    self->len = 0;
    self->idle_state = idle_state;
    self->paused = false;
    part->pulsein = self;
}

bool common_hal_pulseio_pulsein_deinited(pulseio_pulsein_obj_t *self) {
    return self->pin == NULL;
}

void common_hal_pulseio_pulsein_deinit(pulseio_pulsein_obj_t *self) {
    if (common_hal_pulseio_pulsein_deinited(self)) {
        return;
    }
    part_t *part = self->part;
    if (part->pulsein == self) {
        part->pulsein = NULL;
    }
    common_hal_reset_pin(self->pin);
    self->pin = NULL;
    self->part = NULL;
    self->buffer = NULL;
}

void desktop_pulsein_push(pulseio_pulsein_obj_t *self, const uint16_t *pulses, size_t count) {
    if (self->paused) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (self->len < self->maxlen) {
            self->len++;
        } else {
            self->start = (self->start + 1) % self->maxlen;
        }
        self->buffer[(self->start + self->len - 1) % self->maxlen] = pulses[i];
    }
}

void common_hal_pulseio_pulsein_pause(pulseio_pulsein_obj_t *self) {
    self->paused = true;
}

void common_hal_pulseio_pulsein_resume(pulseio_pulsein_obj_t *self, uint16_t trigger_duration) {
    // A trigger pulse is the program's own output: nobody is there to answer it.
    (void)trigger_duration;
    self->paused = false;
}

void common_hal_pulseio_pulsein_clear(pulseio_pulsein_obj_t *self) {
    self->start = 0;
    self->len = 0;
}

uint16_t common_hal_pulseio_pulsein_popleft(pulseio_pulsein_obj_t *self) {
    if (self->len == 0) {
        mp_raise_IndexError_varg(MP_ERROR_TEXT("pop from empty %q"), MP_QSTR_PulseIn);
    }
    uint16_t value = self->buffer[self->start];
    self->start = (self->start + 1) % self->maxlen;
    self->len--;
    return value;
}

uint16_t common_hal_pulseio_pulsein_get_maxlen(pulseio_pulsein_obj_t *self) {
    return self->maxlen;
}

bool common_hal_pulseio_pulsein_get_paused(pulseio_pulsein_obj_t *self) {
    return self->paused;
}

uint16_t common_hal_pulseio_pulsein_get_len(pulseio_pulsein_obj_t *self) {
    return self->len;
}

uint16_t common_hal_pulseio_pulsein_get_item(pulseio_pulsein_obj_t *self, int16_t index) {
    if (index < 0) {
        index += self->len;
    }
    if (index < 0 || index >= self->len) {
        mp_raise_IndexError_varg(MP_ERROR_TEXT("%q out of range"), MP_QSTR_index);
    }
    return self->buffer[(self->start + index) % self->maxlen];
}
