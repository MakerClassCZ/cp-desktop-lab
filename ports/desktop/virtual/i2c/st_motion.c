// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// LSM6DS33 (accelerometer, still gyro) and LIS3MDL (magnetometer): ST's register files

static void _put16(uint8_t *data, size_t i, uint8_t reg, uint8_t base, double value) {
    double v = round(value);
    int16_t raw = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    data[i] = ((reg - base) & 1) ? (uint8_t)((uint16_t)raw >> 8) : (uint8_t)raw;
}

static void _lsm6ds33_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    i2c_model_st_write(dev, data, len, 0x12, 0x01);
}

static void _lsm6ds33_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    static const double mg_per_count[] = { 0.061, 0.488, 0.122, 0.244 };   // by CTRL1_XL's range
    double scale = 1000 / mg_per_count[(dev->regs[0x10] >> 2) & 3];
    for (size_t i = 0; i < len; i++) {
        uint8_t reg = dev->pointer;
        if (reg == 0x0f) {
            data[i] = 0x69;
        } else if (reg == 0x1e) {
            data[i] = 0x07;             // accelerometer, gyro and temperature ready
        } else if (reg >= 0x28 && reg <= 0x2d) {
            _put16(data, i, reg, 0x28, dev->value[(reg - 0x28) / 2] * scale);
        } else if (reg >= 0x20 && reg <= 0x27) {
            data[i] = 0;                // 25 degrees, the gyro still
        } else {
            data[i] = dev->regs[reg];
        }
        dev->pointer = (dev->pointer + 1) & 0x7f;
    }
}

static void _lis3mdl_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    i2c_model_st_write(dev, data, len, 0x21, 0x04);
}

static void _lis3mdl_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    static const double counts_per_gauss[] = { 6842, 3421, 2281, 1711 };  // by CTRL_REG2's range
    double scale = counts_per_gauss[(dev->regs[0x21] >> 5) & 3] / 100;      // microtesla
    for (size_t i = 0; i < len; i++) {
        uint8_t reg = dev->pointer;
        if (reg == 0x0f) {
            data[i] = 0x3d;
        } else if (reg == 0x27) {
            data[i] = 0x0f;             // STATUS_REG: x, y, z ready
        } else if (reg >= 0x28 && reg <= 0x2d) {
            _put16(data, i, reg, 0x28, dev->value[(reg - 0x28) / 2] * scale);
        } else {
            data[i] = dev->regs[reg];
        }
        dev->pointer = (dev->pointer + 1) & 0x7f;
    }
}

const i2c_virtual_model_t i2c_model_lsm6ds33 = {
    "lsm6ds33", 0x6a, 3, {
        { "x", "g", -4, 4, 0, 2 }, { "y", "g", -4, 4, 0, 2 }, { "z", "g", -4, 4, 1, 2 } },
    _lsm6ds33_write, _lsm6ds33_read
};

const i2c_virtual_model_t i2c_model_lis3mdl = {
    "lis3mdl", 0x1c, 3, {
        { "x", "uT", -400, 400, 20, 1 }, { "y", "uT", -400, 400, 0, 1 }, { "z", "uT", -400, 400, -40, 1 } },
    _lis3mdl_write, _lis3mdl_read
};
