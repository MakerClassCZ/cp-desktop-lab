// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "models.h"

// CST816T: a Hynitron capacitive touch controller with FocalTech's point registers (from 0x02:
// the number of touches, then XH with the event in bits 7-6, XL, YH, YL, weight, misc). From
// 0xa6: firmware version, chip 0xb5 (CST816T), module and project ids, chip type.

static void _cst816_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    if (dev->value[2] <= 0.5) {
        dev->touch_event = 0;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t reg = (uint8_t)(dev->pointer + i);
        switch (reg) {
            case 0xa6:
                data[i] = 0x01;
                break;
            case 0xa7:
                data[i] = 0xb5;
                break;
            case 0xa8:
                data[i] = 0x01;
                break;
            case 0xa9:
                data[i] = 0x01;
                break;
            case 0xaa:
                data[i] = 0xb5;
                break;
            default:
                data[i] = reg < 0x10 ? i2c_model_touch_reg(dev, reg) : dev->regs[reg];
        }
    }
}

const i2c_virtual_model_t i2c_model_cst816 = {
    "cst816", 0x15, 3, { { "x", "", 0, 4095, 0, 0 }, { "y", "", 0, 4095, 0, 0 }, { "touched", "", 0, 1, 0, 0 } },
    i2c_model_plain_write, _cst816_read, .driver = "adafruit_cst8xx", .touch = I2C_TOUCH_PIXELS
};
