// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdio.h>

#include "bus_trace.h"
#include "models.h"

// DRV2605L: a haptic motor driver with byte registers; GO plays the waveform sequence, which goes
// to the bus trace, and is done at once.

static void _drv2605_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    i2c_model_plain_write(dev, data, len);
    if (dev->regs[0x0c] & 1) {
        char text[80];
        int at = snprintf(text, sizeof(text), "%s plays", dev->name);
        for (int i = 0x04; i <= 0x0b && dev->regs[i] != 0 && at < (int)sizeof(text) - 8; i++) {
            at += snprintf(text + at, sizeof(text) - at, " %u", dev->regs[i] & 0x7f);
        }
        bus_trace_text("%s", text);
        dev->regs[0x0c] = 0;
    }
}

static void _drv2605_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t reg = (uint8_t)(dev->pointer + i);
        data[i] = reg == 0x00 ? 0xe0 : dev->regs[reg];   // STATUS: device id 7, a DRV2605L
    }
}

const i2c_virtual_model_t i2c_model_drv2605 = {
    "drv2605", 0x5a, 0, { { NULL } }, _drv2605_write, _drv2605_read, .driver = "adafruit_drv2605"
};
