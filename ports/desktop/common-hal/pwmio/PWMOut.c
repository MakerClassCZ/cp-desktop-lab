// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/pwmio/PWMOut.h"

#include "desktop_pins.h"
#include "parts.h"

pwmout_result_t common_hal_pwmio_pwmout_construct(pwmio_pwmout_obj_t *self, const mcu_pin_obj_t *pin,
    uint16_t duty, uint32_t frequency, bool variable_frequency) {
    if (desktop_pin_backend != &desktop_virtual_backend) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("pwmio needs --board virtual"));
    }
    if (!desktop_pin_is_gpio(pin->number)) {
        return PWMOUT_INVALID_PIN;
    }
    if (frequency == 0) {
        return PWMOUT_INVALID_FREQUENCY;
    }
    part_t *part = parts_for_pin(pin->number, PART_PWM);
    if (part == NULL) {
        return PWMOUT_INTERNAL_RESOURCES_IN_USE;
    }
    common_hal_mcu_pin_claim(pin);
    self->pin = pin;
    self->part = part;
    self->duty = duty;
    self->frequency = frequency;
    self->variable_frequency = variable_frequency;
    parts_pwm(part, duty, frequency, true);
    return PWMOUT_OK;
}

bool common_hal_pwmio_pwmout_deinited(pwmio_pwmout_obj_t *self) {
    return self->pin == NULL;
}

void common_hal_pwmio_pwmout_deinit(pwmio_pwmout_obj_t *self) {
    if (common_hal_pwmio_pwmout_deinited(self)) {
        return;
    }
    parts_pwm(self->part, self->duty, self->frequency, false);
    common_hal_reset_pin(self->pin);
    self->pin = NULL;
    self->part = NULL;
}

void common_hal_pwmio_pwmout_set_duty_cycle(pwmio_pwmout_obj_t *self, uint16_t duty) {
    self->duty = duty;
    parts_pwm(self->part, duty, self->frequency, true);
}

uint16_t common_hal_pwmio_pwmout_get_duty_cycle(pwmio_pwmout_obj_t *self) {
    return self->duty;
}

void common_hal_pwmio_pwmout_set_frequency(pwmio_pwmout_obj_t *self, uint32_t frequency) {
    self->frequency = frequency;
    parts_pwm(self->part, self->duty, frequency, true);
}

uint32_t common_hal_pwmio_pwmout_get_frequency(pwmio_pwmout_obj_t *self) {
    return self->frequency;
}

bool common_hal_pwmio_pwmout_get_variable_frequency(pwmio_pwmout_obj_t *self) {
    return self->variable_frequency;
}

const mcu_pin_obj_t *common_hal_pwmio_pwmout_get_pin(pwmio_pwmout_obj_t *self) {
    return self->pin;
}

#if CIRCUITPY_BULK_RESET
void common_hal_pwmio_pwmout_never_reset(pwmio_pwmout_obj_t *self) {
    common_hal_never_reset_pin(self->pin);
}
#endif
