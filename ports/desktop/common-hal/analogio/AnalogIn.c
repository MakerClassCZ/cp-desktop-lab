// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/analogio/AnalogIn.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "desktop_pins.h"
#include "mcp2221.h"
#include "parts.h"

static void _need_mcp2221(void) {
    if (desktop_pin_backend != &desktop_mcp2221_backend) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("analogio needs --board mcp2221 or virtual"));
    }
}

void common_hal_analogio_analogin_construct(analogio_analogin_obj_t *self, const mcu_pin_obj_t *pin) {
    self->part = NULL;
    if (desktop_pin_backend == &desktop_virtual_backend) {
        // A slider of the board panel.
        if (!desktop_pin_is_gpio(pin->number)) {
            raise_ValueError_invalid_pin();
        }
        part_t *part = parts_for_pin(pin->number, PART_ANALOG);
        if (part == NULL) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("too many parts on the board panel"));
        }
        part->output = false;
        common_hal_mcu_pin_claim(pin);
        self->pin = pin;
        self->part = part;
        return;
    }
    _need_mcp2221();
    // ADC1-ADC3 are GP1-GP3.
    if (pin->number < MCP2221_PIN_GP0 + 1 || pin->number >= MCP2221_PIN_COUNT) {
        raise_ValueError_invalid_pin();
    }
    desktop_pin_backend_raise_if_lost();
    common_hal_mcu_pin_claim(pin);
    self->pin = pin;
    self->channel = pin->number - MCP2221_PIN_GP0;
    mcp2221_set_designation(pin->number - MCP2221_PIN_GP0, MCP2221_ADC);
    desktop_pin_backend_raise_if_lost();
}

bool common_hal_analogio_analogin_deinited(analogio_analogin_obj_t *self) {
    return self->pin == NULL;
}

void common_hal_analogio_analogin_deinit(analogio_analogin_obj_t *self) {
    if (common_hal_analogio_analogin_deinited(self)) {
        return;
    }
    // Back to a GPIO input.
    common_hal_reset_pin(self->pin);
    self->pin = NULL;
    self->part = NULL;
}

uint16_t common_hal_analogio_analogin_get_value(analogio_analogin_obj_t *self) {
    if (self->part != NULL) {
        part_t *part = self->part;
        return part->detached ? 0 : part->value;
    }
    uint16_t value = 0;
    mcp2221_adc_read(self->channel, &value);
    desktop_pin_backend_raise_if_lost();
    // 10 bits to 16: 1023 reads as 65535.
    return (uint16_t)((value << 6) | (value >> 4));
}

float common_hal_analogio_analogin_get_reference_voltage(analogio_analogin_obj_t *self) {
    (void)self;
    return 3.3f;
}
