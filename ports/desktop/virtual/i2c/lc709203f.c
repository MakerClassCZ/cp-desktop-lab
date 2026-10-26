// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// LC709203F: a battery fuel gauge; 16-bit words, little-endian, each with a CRC-8

static uint8_t _crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void _lc709203f_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    dev->pointer = data[0];
    if (len >= 3) {
        dev->words[dev->pointer] = (uint16_t)(data[1] | data[2] << 8);
    }
}

static void _lc709203f_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    uint16_t word;
    switch (dev->pointer) {
        case 0x09:
            word = (uint16_t)lround(dev->value[1] * 1000);     // mV
            break;
        case 0x0d:
            word = (uint16_t)lround(dev->value[0]);            // %
            break;
        case 0x0f:
            word = (uint16_t)lround(dev->value[0] * 10);       // 0.1 %
            break;
        case 0x11:
            word = 0x2717;                                      // the IC version
            break;
        case 0x08:
            word = dev->words[0x08] ? dev->words[0x08] : 2982;  // 0.1 K: 25 degrees
            break;
        default:
            word = dev->words[dev->pointer];
    }
    uint8_t frame[5] = { (uint8_t)(dev->address << 1), dev->pointer, (uint8_t)(dev->address << 1 | 1),
                         (uint8_t)word, (uint8_t)(word >> 8) };
    const uint8_t reply[3] = { frame[3], frame[4], _crc8(frame, 5) };
    for (size_t i = 0; i < len; i++) {
        data[i] = i < 3 ? reply[i] : 0xff;
    }
}

const i2c_virtual_model_t i2c_model_lc709203f = {
    "lc709203f", 0x0b, 2, { { "percent", "%", 0, 100, 80, 1 }, { "volt", "V", 3, 4.2, 3.9, 2 } },
    _lc709203f_write, _lc709203f_read
};
