// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdio.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/pulseio/PulseOut.h"

#include "bus_trace.h"
#include "desktop_pins.h"

void common_hal_pulseio_pulseout_construct(pulseio_pulseout_obj_t *self,
    const mcu_pin_obj_t *pin, uint32_t frequency, uint16_t duty_cycle) {
    (void)duty_cycle;
    bool output, level;
    if (!desktop_pin_is_gpio(pin->number) || !desktop_virtual_pin(pin->number, &output, &level)) {
        mp_raise_ValueError(MP_ERROR_TEXT("pulseio needs the virtual board's pins"));
    }
    common_hal_mcu_pin_claim(pin);
    self->pin = pin;
    self->frequency = frequency;
}

bool common_hal_pulseio_pulseout_deinited(pulseio_pulseout_obj_t *self) {
    return self->pin == NULL;
}

void common_hal_pulseio_pulseout_deinit(pulseio_pulseout_obj_t *self) {
    if (common_hal_pulseio_pulseout_deinited(self)) {
        return;
    }
    common_hal_reset_pin(self->pin);
    self->pin = NULL;
}

void common_hal_pulseio_pulseout_send(pulseio_pulseout_obj_t *self, uint16_t *pulses, uint16_t len) {
    // "PULSEOUT GPIOn 38000 Hz 3: 9000 4500 560", the durations in microseconds.
    uint32_t total = 0;
    char line[560];
    int n = snprintf(line, sizeof(line), "PULSEOUT GPIO%u %lu Hz %u:", (unsigned)self->pin->number,
        (unsigned long)self->frequency, (unsigned)len);
    for (uint16_t i = 0; i < len; i++) {
        total += pulses[i];
        if (n < (int)sizeof(line) - 8) {
            n += snprintf(line + n, sizeof(line) - n, " %u", (unsigned)pulses[i]);
        }
    }
    bus_trace_text("%s", line);
    mp_hal_delay_ms((total + 999) / 1000);
}
