// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/touchio/TouchIn.h"

#include "desktop_pins.h"
#include "parts.h"

// The raw values a pad gives: untouched under the threshold, touched well above it.
#define UNTOUCHED 2000
#define TOUCHED 4000

void common_hal_touchio_touchin_construct(touchio_touchin_obj_t *self, const mcu_pin_obj_t *pin, digitalio_pull_t pull) {
    (void)pull;
    if (desktop_pin_backend != &desktop_virtual_backend) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("touchio needs --board virtual"));
    }
    if (!desktop_pin_is_gpio(pin->number)) {
        raise_ValueError_invalid_pin();
    }
    part_t *part = parts_for_pin(pin->number, PART_TOUCH);
    if (part == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("too many parts on the board panel"));
    }
    common_hal_mcu_pin_claim(pin);
    self->pin = pin;
    self->part = part;
    self->threshold = UNTOUCHED + 100;
}

bool common_hal_touchio_touchin_deinited(touchio_touchin_obj_t *self) {
    return self->pin == NULL;
}

void common_hal_touchio_touchin_deinit(touchio_touchin_obj_t *self) {
    if (common_hal_touchio_touchin_deinited(self)) {
        return;
    }
    common_hal_reset_pin(self->pin);
    self->pin = NULL;
    self->part = NULL;
}

uint16_t common_hal_touchio_touchin_get_raw_value(touchio_touchin_obj_t *self) {
    return ((part_t *)self->part)->on ? TOUCHED : UNTOUCHED;
}

bool common_hal_touchio_touchin_get_value(touchio_touchin_obj_t *self) {
    return common_hal_touchio_touchin_get_raw_value(self) > self->threshold;
}

uint16_t common_hal_touchio_touchin_get_threshold(touchio_touchin_obj_t *self) {
    return self->threshold;
}

void common_hal_touchio_touchin_set_threshold(touchio_touchin_obj_t *self, uint16_t new_threshold) {
    self->threshold = new_threshold;
}
