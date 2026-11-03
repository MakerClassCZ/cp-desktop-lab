// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// AHT10, AHT20: commands it needs no answer to; a read is the status (calibrated, never busy),
// then humidity and temperature, 20 bits each.

static void _aht20_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    (void)dev;
    (void)data;
    (void)len;
}

static void _aht20_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    uint32_t hum = (uint32_t)lround(dev->value[1] * 0x100000 / 100);
    uint32_t temp = (uint32_t)lround((dev->value[0] + 50) * 0x100000 / 200);
    hum = hum > 0xfffff ? 0xfffff : hum;
    temp = temp > 0xfffff ? 0xfffff : temp;
    const uint8_t frame[6] = {
        0x18, (uint8_t)(hum >> 12), (uint8_t)(hum >> 4), (uint8_t)((hum & 0xf) << 4 | temp >> 16),
        (uint8_t)(temp >> 8), (uint8_t)temp,
    };
    for (size_t i = 0; i < len; i++) {
        data[i] = i < sizeof(frame) ? frame[i] : 0;
    }
}

const i2c_virtual_model_t i2c_model_aht20 = {
    "aht20", 0x38, 2, { { "temp", "C", -40, 85, 22, 2 }, { "hum", "%", 0, 100, 45, 1 } },
    _aht20_write, _aht20_read, .driver = "adafruit_ahtx0"
};
