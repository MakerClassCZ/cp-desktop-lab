// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/digitalio/DigitalInOut.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "desktop_pins.h"

static digitalinout_result_t _configure(digitalio_digitalinout_obj_t *self, bool output, bool open_drain, digitalio_pull_t pull) {
    digitalinout_result_t result = desktop_pin_backend->configure(self->pin->number, output, open_drain, pull);
    if (result == DIGITALINOUT_OK) {
        self->output = output;
        self->open_drain = open_drain;
        self->pull = pull;
    }
    desktop_pin_backend_raise_if_lost();
    return result;
}

digitalinout_result_t common_hal_digitalio_digitalinout_construct(
    digitalio_digitalinout_obj_t *self, const mcu_pin_obj_t *pin) {
    if (!desktop_pin_is_gpio(pin->number)
        || (desktop_pin_backend->pin_ok != NULL && !desktop_pin_backend->pin_ok(pin->number))) {
        raise_ValueError_invalid_pin();
    }
    self->pin = pin;
    digitalinout_result_t result = _configure(self, false, false, PULL_NONE);
    // A pin that cannot be set up stays free for the next try.
    if (result == DIGITALINOUT_OK) {
        common_hal_mcu_pin_claim(pin);
    } else {
        self->pin = NULL;
    }
    return result;
}

void common_hal_digitalio_digitalinout_never_reset(digitalio_digitalinout_obj_t *self) {
    common_hal_never_reset_pin(self->pin);
}

bool common_hal_digitalio_digitalinout_deinited(digitalio_digitalinout_obj_t *self) {
    return self->pin == NULL;
}

void common_hal_digitalio_digitalinout_deinit(digitalio_digitalinout_obj_t *self) {
    if (common_hal_digitalio_digitalinout_deinited(self)) {
        return;
    }
    common_hal_reset_pin(self->pin);
    self->pin = NULL;
}

digitalinout_result_t common_hal_digitalio_digitalinout_switch_to_input(
    digitalio_digitalinout_obj_t *self, digitalio_pull_t pull) {
    return _configure(self, false, self->open_drain, pull);
}

digitalinout_result_t common_hal_digitalio_digitalinout_switch_to_output(
    digitalio_digitalinout_obj_t *self, bool value, digitalio_drive_mode_t drive_mode) {
    // The level first, so the pin never drives the old one.
    desktop_pin_backend->write(self->pin->number, value);
    return _configure(self, true, drive_mode == DRIVE_MODE_OPEN_DRAIN, PULL_NONE);
}

digitalio_direction_t common_hal_digitalio_digitalinout_get_direction(
    digitalio_digitalinout_obj_t *self) {
    return self->output ? DIRECTION_OUTPUT : DIRECTION_INPUT;
}

void common_hal_digitalio_digitalinout_set_value(digitalio_digitalinout_obj_t *self, bool value) {
    desktop_pin_backend->write(self->pin->number, value);
    desktop_pin_backend_raise_if_lost();
}

bool common_hal_digitalio_digitalinout_get_value(digitalio_digitalinout_obj_t *self) {
    bool level = false;
    desktop_pin_backend->read(self->pin->number, &level);
    desktop_pin_backend_raise_if_lost();
    return level;
}

digitalinout_result_t common_hal_digitalio_digitalinout_set_drive_mode(
    digitalio_digitalinout_obj_t *self, digitalio_drive_mode_t drive_mode) {
    return _configure(self, self->output, drive_mode == DRIVE_MODE_OPEN_DRAIN, self->pull);
}

digitalio_drive_mode_t common_hal_digitalio_digitalinout_get_drive_mode(
    digitalio_digitalinout_obj_t *self) {
    return self->open_drain ? DRIVE_MODE_OPEN_DRAIN : DRIVE_MODE_PUSH_PULL;
}

digitalinout_result_t common_hal_digitalio_digitalinout_set_pull(
    digitalio_digitalinout_obj_t *self, digitalio_pull_t pull) {
    return _configure(self, self->output, self->open_drain, pull);
}

digitalio_pull_t common_hal_digitalio_digitalinout_get_pull(digitalio_digitalinout_obj_t *self) {
    return self->output ? PULL_NONE : self->pull;
}
