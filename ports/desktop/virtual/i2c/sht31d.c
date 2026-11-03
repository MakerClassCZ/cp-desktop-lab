// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// SHT31-D: 16-bit commands; a measurement reads temperature and humidity, each with a CRC

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
    uint8_t frame[6];
    i2c_model_sensirion_word(frame, first);
    i2c_model_sensirion_word(frame + 3, second);
    for (size_t i = 0; i < len; i++) {
        data[i] = frame[i % 6];             // a periodic fetch reads the frame over and over
    }
}

const i2c_virtual_model_t i2c_model_sht31d = {
    "sht31d", 0x44, 2, { { "temp", "C", -40, 125, 22, 2 }, { "hum", "%", 0, 100, 45, 1 } },
    _sht31d_write, _sht31d_read, .driver = "adafruit_sht31d"
};
