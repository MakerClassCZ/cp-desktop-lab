// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "bus_trace.h"
#include "models.h"

// PCA9685: 16 PWM outputs with byte registers, four per output from 0x06 (on and off counts of
// 4096); an output that changes goes to the bus trace with its time high, as a servo sees it.

static void _pca9685_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    uint8_t before[64];
    for (int i = 0; i < 64; i++) {
        before[i] = dev->regs[0x06 + i];
    }
    i2c_model_plain_write(dev, data, len);
    unsigned prescale = dev->regs[0xfe] ? dev->regs[0xfe] : 0x1e;
    double hz = 25e6 / 4096 / (prescale + 1);
    for (int ch = 0; ch < 16; ch++) {
        const uint8_t *r = &dev->regs[0x06 + 4 * ch];
        if (r[0] == before[4 * ch] && r[1] == before[4 * ch + 1] && r[2] == before[4 * ch + 2] && r[3] == before[4 * ch + 3]) {
            continue;
        }
        unsigned on = (r[0] | r[1] << 8) & 0x1fff, off = (r[2] | r[3] << 8) & 0x1fff;
        unsigned high = off & 0x1000 ? 0 : on & 0x1000 ? 4096 : (off - on) & 0xfff;
        bus_trace_text("%s %d %u/4096 at %.0f Hz: %.2f ms high", dev->name, ch, high, hz, high / 4096.0 * 1000.0 / hz);
    }
}

static void _pca9685_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t reg = (uint8_t)(dev->pointer + i);
        data[i] = reg == 0xfe && dev->regs[reg] == 0 ? 0x1e : dev->regs[reg];   // PRESCALE: 200 Hz
    }
}

const i2c_virtual_model_t i2c_model_pca9685 = {
    "pca9685", 0x40, 0, { { NULL } }, _pca9685_write, _pca9685_read, .driver = "adafruit_pca9685"
};
