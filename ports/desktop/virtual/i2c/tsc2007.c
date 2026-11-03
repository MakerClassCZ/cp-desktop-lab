// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// TSC2007: a resistive touch screen's controller. A command byte names what to measure in its
// high four bits (12 x, 13 y, 14 the pressure z1); the read is that 12-bit value, shifted left
// by four. Untouched, the pressure is 0.

static void _tsc2007_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    (void)len;
    dev->command = data[0];
}

static void _tsc2007_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    int function = dev->command >> 4;
    double value = function == 12 ? dev->value[0] : function == 13 ? dev->value[1] : function == 14 ? dev->value[2] : 0;
    uint16_t raw = (uint16_t)lround(value);
    raw = raw > 4095 ? 4095 : raw;
    for (size_t i = 0; i < len; i++) {
        data[i] = i == 0 ? (uint8_t)(raw >> 4) : i == 1 ? (uint8_t)(raw << 4) : 0;
    }
}

const i2c_virtual_model_t i2c_model_tsc2007 = {
    "tsc2007", 0x48, 3, { { "x", "", 0, 4095, 2048, 0 }, { "y", "", 0, 4095, 2048, 0 }, { "pressure", "", 0, 4095, 0, 0 } },
    _tsc2007_write, _tsc2007_read, .driver = "adafruit_tsc2007"
};
