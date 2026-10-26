// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/digitalio/Pull.h"

#include "desktop_pins.h"

// One bit per pin number, the serial and bus placeholder pins (30-36) included.
static uint64_t _claimed;
static uint64_t _never_reset;

void reset_all_pins(void) {
    _claimed &= _never_reset;
    desktop_pin_backend->reset((uint32_t)_never_reset);
}

void common_hal_reset_pin(const mcu_pin_obj_t *pin) {
    if (pin != NULL) {
        common_hal_mcu_pin_reset_number(pin->number);
    }
}

void common_hal_mcu_pin_reset_number(uint8_t pin_no) {
    if (pin_no < 64) {
        _never_reset &= ~(1ull << pin_no);
        _claimed &= ~(1ull << pin_no);
    }
    // A bus pin of a chip's own engine (the MCP2221's SCL and SDA) is no GPIO to reset.
    if (desktop_pin_is_gpio(pin_no) && (desktop_pin_backend->pin_ok == NULL || desktop_pin_backend->pin_ok(pin_no))) {
        if (desktop_pin_backend->release != NULL) {
            desktop_pin_backend->release(pin_no);
        } else {
            desktop_pin_backend->configure(pin_no, false, false, PULL_NONE);
        }
    }
}

void common_hal_never_reset_pin(const mcu_pin_obj_t *pin) {
    _never_reset |= 1ull << pin->number;
}

bool common_hal_mcu_pin_is_free(const mcu_pin_obj_t *pin) {
    return !(_claimed & (1ull << pin->number));
}

uint8_t common_hal_mcu_pin_number(const mcu_pin_obj_t *pin) {
    return pin->number;
}

void common_hal_mcu_pin_claim(const mcu_pin_obj_t *pin) {
    _claimed |= 1ull << pin->number;
}
