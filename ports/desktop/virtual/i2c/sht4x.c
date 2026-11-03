// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// SHT40, SHT41, SHT45: one-byte commands; a measurement (any precision, heater or not) reads
// temperature and humidity, each with a CRC; humidity is -6 + 125 * raw / 65535.

static void _sht4x_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    (void)len;
    dev->command = data[0];
}

static void _sht4x_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    uint16_t first, second;
    if (dev->command == 0x89) {             // the serial number
        first = 0x1234;
        second = 0x5678;
    } else {
        first = (uint16_t)lround((dev->value[0] + 45) * 65535 / 175);
        second = (uint16_t)lround((dev->value[1] + 6) * 65535 / 125);
    }
    uint8_t frame[6];
    i2c_model_sensirion_word(frame, first);
    i2c_model_sensirion_word(frame + 3, second);
    for (size_t i = 0; i < len; i++) {
        data[i] = i < sizeof(frame) ? frame[i] : 0;
    }
}

const i2c_virtual_model_t i2c_model_sht4x = {
    "sht4x", 0x44, 2, { { "temp", "C", -40, 125, 22, 2 }, { "hum", "%", 0, 100, 45, 1 } },
    _sht4x_write, _sht4x_read, .driver = "adafruit_sht4x"
};
