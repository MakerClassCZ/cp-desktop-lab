// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// neopixel_write: the pixels appear in the NeoPixel window (neopixel_emu.c) on every board, and
// go out to real ones where the pin backend can send them (a Raspberry Pi's MOSI through spidev).

#include "py/runtime.h"
#include "shared-bindings/neopixel_write/__init__.h"

#include "desktop_pins.h"
#include "neopixel_emu.h"

void common_hal_neopixel_write(const digitalio_digitalinout_obj_t *gpio, uint8_t *pixels, uint32_t numBytes) {
    if (desktop_pin_backend->neopixel != NULL) {
        const char *why = NULL;
        if (!desktop_pin_backend->neopixel(gpio->pin->number, pixels, numBytes, &why)) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("%s"), why != NULL ? why : "NeoPixel write failed");
        }
    }
    neopixel_emu_write(gpio->pin->number, pixels, numBytes);
}
