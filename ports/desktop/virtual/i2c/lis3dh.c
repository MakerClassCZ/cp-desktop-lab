// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// LIS3DH: an accelerometer, x, y and z in g; left-justified 16-bit counts by its range

static void _lis3dh_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    i2c_model_st_write(dev, data, len, 0x24, 0x80);
}

static void _lis3dh_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    static const double dividers[] = { 16380, 8190, 4096, 1365 };   // the library's, by CTRL4's range
    double divider = dividers[(dev->regs[0x23] >> 4) & 3];
    for (size_t i = 0; i < len; i++) {
        uint8_t reg = dev->pointer;
        if (reg == 0x0f) {
            data[i] = 0x33;
        } else if (reg >= 0x28 && reg <= 0x2d) {
            double v = round(dev->value[(reg - 0x28) / 2] * divider);
            int16_t raw = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            data[i] = (reg & 1) ? (uint8_t)((uint16_t)raw >> 8) : (uint8_t)raw;
        } else if (reg == 0x31 || reg == 0x39 || (reg >= 0x08 && reg <= 0x0d)) {
            data[i] = 0;                // no interrupt, no click, the ADC at 0
        } else {
            data[i] = dev->regs[reg];
        }
        dev->pointer = (dev->pointer + 1) & 0x7f;
    }
}

const i2c_virtual_model_t i2c_model_lis3dh = {
    "lis3dh", 0x18, 3, {
        { "x", "g", -4, 4, 0, 2 }, { "y", "g", -4, 4, 0, 2 }, { "z", "g", -4, 4, 1, 2 }
    },
    _lis3dh_write, _lis3dh_read
};
