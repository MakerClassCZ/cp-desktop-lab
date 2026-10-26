// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/rotaryio/IncrementalEncoder.h"

#include "desktop_pins.h"
#include "parts.h"

void common_hal_rotaryio_incrementalencoder_construct(rotaryio_incrementalencoder_obj_t *self,
    const mcu_pin_obj_t *pin_a, const mcu_pin_obj_t *pin_b) {
    bool output, level;
    if (!desktop_pin_is_gpio(pin_a->number) || !desktop_virtual_pin(pin_a->number, &output, &level)) {
        mp_raise_ValueError(MP_ERROR_TEXT("rotaryio needs the virtual board's pins"));
    }
    part_t *part = parts_for_pin(pin_a->number, PART_ENCODER);
    if (part == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("too many parts"));
    }
    common_hal_mcu_pin_claim(pin_a);
    common_hal_mcu_pin_claim(pin_b);
    self->pin_a = pin_a;
    self->pin_b = pin_b;
    self->part = part;
    self->base_steps = part->steps;
    self->base_position = 0;
    self->divisor = 4;
}

bool common_hal_rotaryio_incrementalencoder_deinited(rotaryio_incrementalencoder_obj_t *self) {
    return self->pin_a == NULL;
}

void common_hal_rotaryio_incrementalencoder_mark_deinit(rotaryio_incrementalencoder_obj_t *self) {
    self->pin_a = NULL;
}

void common_hal_rotaryio_incrementalencoder_deinit(rotaryio_incrementalencoder_obj_t *self) {
    if (common_hal_rotaryio_incrementalencoder_deinited(self)) {
        return;
    }
    common_hal_reset_pin(self->pin_a);
    common_hal_reset_pin(self->pin_b);
    self->part = NULL;
    common_hal_rotaryio_incrementalencoder_mark_deinit(self);
}

mp_int_t common_hal_rotaryio_incrementalencoder_get_position(rotaryio_incrementalencoder_obj_t *self) {
    int32_t steps = ((part_t *)self->part)->steps - self->base_steps;
    return self->base_position + steps / self->divisor;
}

void common_hal_rotaryio_incrementalencoder_set_position(rotaryio_incrementalencoder_obj_t *self, mp_int_t new_position) {
    self->base_steps = ((part_t *)self->part)->steps;
    self->base_position = new_position;
}

mp_int_t common_hal_rotaryio_incrementalencoder_get_divisor(rotaryio_incrementalencoder_obj_t *self) {
    return self->divisor;
}

void common_hal_rotaryio_incrementalencoder_set_divisor(rotaryio_incrementalencoder_obj_t *self, mp_int_t new_divisor) {
    // Counted from here on with the new divisor.
    common_hal_rotaryio_incrementalencoder_set_position(self, common_hal_rotaryio_incrementalencoder_get_position(self));
    self->divisor = new_divisor;
}
