// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// SHT31-D: 16-bit commands; a measurement reads temperature and humidity, each with a CRC

static uint8_t _sht_crc(uint8_t hi, uint8_t lo) {
    uint8_t crc = 0xff;
    const uint8_t bytes[2] = { hi, lo };
    for (int k = 0; k < 2; k++) {
        crc ^= bytes[k];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void _sht31d_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    if (len >= 2) {
        dev->words[0] = (uint16_t)(data[0] << 8 | data[1]);
    }
}

static void _sht31d_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    uint16_t first, second;
    if (dev->words[0] == 0xf32d) {          // the status register
        first = second = 0;
    } else if (dev->words[0] == 0x3780) {   // the serial number
        first = 0x1234;
        second = 0x5678;
    } else {
        first = (uint16_t)lround((dev->value[0] + 45) * 65535 / 175);
        second = (uint16_t)lround(dev->value[1] * 65535 / 100);
    }
    const uint8_t frame[6] = {
        (uint8_t)(first >> 8), (uint8_t)first, _sht_crc((uint8_t)(first >> 8), (uint8_t)first),
        (uint8_t)(second >> 8), (uint8_t)second, _sht_crc((uint8_t)(second >> 8), (uint8_t)second),
    };
    for (size_t i = 0; i < len; i++) {
        data[i] = frame[i % 6];             // a periodic fetch reads the frame over and over
    }
}

const i2c_virtual_model_t i2c_model_sht31d = {
    "sht31d", 0x44, 2, { { "temp", "C", -40, 125, 22, 2 }, { "hum", "%", 0, 100, 45, 1 } },
    _sht31d_write, _sht31d_read
};
