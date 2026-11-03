// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// MAX17048: a battery fuel gauge with 16-bit registers, most significant byte first. Its reset
// command (0x5400 to 0xfe) is not acknowledged, as the chip resets.

static void _max17048_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    dev->pointer = data[0];
    if (len == 2) {
        dev->regs[dev->pointer] = data[1];
        uint8_t reg = dev->pointer & 0xfe;   // a byte of a word: the one it is
        dev->words[reg] = (uint16_t)(dev->regs[reg] << 8 | dev->regs[reg + 1]);
    } else if (len >= 3) {
        if (dev->pointer == 0xfe && data[1] == 0x54 && data[2] == 0x00) {
            dev->nack = true;
            return;
        }
        dev->words[dev->pointer] = (uint16_t)(data[1] << 8 | data[2]);
        dev->regs[dev->pointer] = data[1];
        dev->regs[(uint8_t)(dev->pointer + 1)] = data[2];
    }
}

static void _max17048_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    uint8_t reg = dev->pointer & 0xfe;
    uint16_t word;
    switch (reg) {
        case 0x02:                          // VCELL, 78.125 uV
            word = (uint16_t)lround(dev->value[1] * 1e6 / 78.125);
            break;
        case 0x04:                          // SOC, 1/256 %
            word = (uint16_t)lround(dev->value[0] * 256);
            break;
        case 0x08:                          // the version
            word = 0x0012;
            break;
        case 0x0c:                          // CONFIG, its default
            word = dev->words[0x0c] ? dev->words[0x0c] : 0x971c;
            break;
        case 0x18:                          // VRESET, its default 3 V, and the chip id 0
            word = dev->words[0x18] ? dev->words[0x18] : 0x9600;
            break;
        default:
            word = dev->words[reg];
    }
    // A one-byte register (MODE, STATUS) reads from its own byte of the word.
    bool low = (dev->pointer & 1) != 0;
    for (size_t i = 0; i < len; i++) {
        data[i] = (i + low) % 2 == 0 ? (uint8_t)(word >> 8) : (uint8_t)word;
    }
}

const i2c_virtual_model_t i2c_model_max17048 = {
    "max17048", 0x36, 2, { { "percent", "%", 0, 100, 80, 1 }, { "volt", "V", 3, 4.2, 3.9, 2 } },
    _max17048_write, _max17048_read, .driver = "adafruit_max1704x"
};
