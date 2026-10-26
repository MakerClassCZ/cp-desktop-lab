// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/analogio/AnalogOut.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "bus_trace.h"
#include "desktop_pins.h"
#include "mcp2221.h"
#include "panel.h"
#include "parts.h"

static bool _in_use;

void common_hal_analogio_analogout_construct(analogio_analogout_obj_t *self, const mcu_pin_obj_t *pin) {
    self->part = NULL;
    if (desktop_pin_backend == &desktop_virtual_backend) {
        // A bar of the board panel.
        if (!desktop_pin_is_gpio(pin->number)) {
            raise_ValueError_invalid_pin();
        }
        part_t *part = parts_for_pin(pin->number, PART_ANALOG);
        if (part == NULL) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("too many parts on the board panel"));
        }
        part->output = true;
        part->value = 0;
        panel_changed();
        common_hal_mcu_pin_claim(pin);
        self->pin = pin;
        self->part = part;
        return;
    }
    if (desktop_pin_backend != &desktop_mcp2221_backend) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("analogio needs --board mcp2221 or virtual"));
    }
    // The one DAC comes out on GP2 or GP3.
    if (pin->number < MCP2221_PIN_GP0 + 2 || pin->number >= MCP2221_PIN_COUNT) {
        raise_ValueError_invalid_pin();
    }
    if (_in_use) {
        mp_raise_ValueError(MP_ERROR_TEXT("DAC already in use"));
    }
    desktop_pin_backend_raise_if_lost();
    common_hal_mcu_pin_claim(pin);
    self->pin = pin;
    _in_use = true;
    mcp2221_dac_set(0);
    mcp2221_set_designation(pin->number - MCP2221_PIN_GP0, MCP2221_DAC);
    desktop_pin_backend_raise_if_lost();
}

bool common_hal_analogio_analogout_deinited(analogio_analogout_obj_t *self) {
    return self->pin == NULL;
}

void common_hal_analogio_analogout_deinit(analogio_analogout_obj_t *self) {
    if (common_hal_analogio_analogout_deinited(self)) {
        return;
    }
    common_hal_reset_pin(self->pin);
    self->pin = NULL;
    if (self->part != NULL) {
        ((part_t *)self->part)->output = false;
        self->part = NULL;
        panel_changed();
        return;
    }
    _in_use = false;
}

void common_hal_analogio_analogout_set_value(analogio_analogout_obj_t *self, uint16_t value) {
    part_t *part = self->part;
    if (part != NULL) {
        unsigned mv = (unsigned)((uint32_t)value * 3300 / 65535);
        part->value = value;
        bus_trace_text("ANALOG %s = %u (%u.%03u V)", part->name, (unsigned)value, mv / 1000, mv % 1000);
        panel_changed();
        return;
    }
    // 16 bits to the DAC's 5.
    mcp2221_dac_set((uint8_t)(value >> 11));
    desktop_pin_backend_raise_if_lost();
}
