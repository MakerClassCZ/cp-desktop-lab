// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// FT6206: a FocalTech capacitive touch controller, one point of it. Registers from 0: mode,
// gesture, the number of touches, then per point XH (event in bits 7-6, x 11-8), XL, YH (id in
// bits 7-4, y 11-8), YL, weight, misc; a point not touched reads 0xff. The ids from 0xa1: library
// version, chip 0x06, firmware, FocalTech's vendor id 0x11.

uint8_t i2c_model_touch_reg(i2c_virtual_device_t *dev, uint8_t reg) {
    bool down = dev->value[2] > 0.5;
    unsigned x = (unsigned)lround(dev->value[0]) & 0xfff, y = (unsigned)lround(dev->value[1]) & 0xfff;
    // The first read of a touch says it went down (0), the next ones that it is in contact (2).
    uint8_t event = dev->touch_event == 1 ? 2 : 0;
    switch (reg) {
        case 0x02:
            return down ? 1 : 0;
        case 0x03:
            if (down) {
                dev->touch_event = 1;
            }
            return down ? (uint8_t)(event << 6 | x >> 8) : 0xff;
        case 0x04:
            return down ? (uint8_t)x : 0xff;
        case 0x05:
            return down ? (uint8_t)(y >> 8) : 0xff;
        case 0x06:
            return down ? (uint8_t)y : 0xff;
        case 0x07:
            return down ? 0x40 : 0xff;
        case 0x08:
            return down ? 0x00 : 0xff;
        default:
            // Mode, gesture, and a second point that is never there.
            return reg < 0x0f && reg > 0x08 ? 0xff : 0x00;
    }
}

static void _ft6206_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    if (dev->value[2] <= 0.5) {
        dev->touch_event = 0;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t reg = (uint8_t)(dev->pointer + i);
        switch (reg) {
            case 0x80:                      // touch threshold
                data[i] = dev->regs[reg] ? dev->regs[reg] : 128;
                break;
            case 0x88:                      // point rate, Hz
                data[i] = dev->regs[reg] ? dev->regs[reg] : 60;
                break;
            case 0xa1:
                data[i] = 0x30;
                break;
            case 0xa2:
                data[i] = 0x03;
                break;
            case 0xa3:
                data[i] = 0x06;
                break;
            case 0xa6:
                data[i] = 0x10;
                break;
            case 0xa8:
                data[i] = 0x11;
                break;
            case 0xaf:
                data[i] = 0x01;
                break;
            default:
                data[i] = reg < 0x10 ? i2c_model_touch_reg(dev, reg) : dev->regs[reg];
        }
    }
}

const i2c_virtual_model_t i2c_model_ft6206 = {
    "ft6206", 0x38, 3, { { "x", "", 0, 4095, 0, 0 }, { "y", "", 0, 4095, 0, 0 }, { "touched", "", 0, 1, 0, 0 } },
    i2c_model_plain_write, _ft6206_read, .driver = "adafruit_focaltouch", .touch = I2C_TOUCH_PIXELS
};
