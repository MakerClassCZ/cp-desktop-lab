// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// BME280: temperature, humidity and pressure, raw counts found through the compensation

// The datasheet's example calibration.
static const double T1 = 27504, T2 = 26435, T3 = -1000;
static const double P[9] = { 36477, -10685, 3024, 2855, 140, -7, 15500, -14600, 6000 };
static const double H1 = 75, H2 = 362, H3 = 0, H4 = 313, H5 = 50, H6 = 30;

// The library's compensation (adafruit_bme280), in doubles.
static int _bme_t_fine(double at) {
    double v1 = (at / 16384.0 - T1 / 1024.0) * T2;
    double v2 = (at / 131072.0 - T1 / 8192.0) * (at / 131072.0 - T1 / 8192.0) * T3;
    return (int)(v1 + v2);
}

static double _bme_pressure(double ap, int t_fine) {
    double v1 = t_fine / 2.0 - 64000.0;
    double v2 = v1 * v1 * P[5] / 32768.0 + v1 * P[4] * 2.0;
    v2 = v2 / 4.0 + P[3] * 65536.0;
    v1 = (P[2] * v1 * v1 / 524288.0 + P[1] * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * P[0];
    double p = ((1048576.0 - ap - v2 / 4096.0) * 6250.0) / v1;
    return (p + (P[8] * p * p / 2147483648.0 + p * P[7] / 32768.0 + P[6]) / 16.0) / 100.0;
}

static double _bme_humidity(double ah, int t_fine) {
    double v1 = t_fine - 76800.0;
    double v5 = 1.0 + H3 / 67108864.0 * v1;
    double v6 = (ah - (H4 * 64.0 + H5 / 16384.0 * v1)) * (H2 / 65536.0) * (v5 * (1.0 + H6 / 67108864.0 * v1 * v5));
    return v6 * (1.0 - H1 * v6 / 524288.0);
}

// The count in [0, top] whose result is nearest the target; f rises with it, or falls.
static uint32_t _invert(double (*f)(double, int), int t_fine, double target, uint32_t top, bool rising) {
    uint32_t lo = 0, hi = top;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if ((f(mid, t_fine) < target) == rising) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo > 0 && fabs(f(lo - 1, t_fine) - target) < fabs(f(lo, t_fine) - target)) {
        lo--;
    }
    return lo;
}

static double _bme_temperature(double at, int unused) {
    (void)unused;
    return _bme_t_fine(at) / 5120.0;
}

// A BME280 (chip id 0x60) or a BMP280 (0x58, no humidity: hum < 0) reading out its registers.
static void _bmx280_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len, uint8_t chip_id,
    double temperature, double humidity, double pressure) {
    uint8_t calib[0x100] = { 0 };
    int16_t c[12] = { (int16_t)(uint16_t)T1, (int16_t)T2, (int16_t)T3, (int16_t)(uint16_t)P[0], (int16_t)P[1],
                      (int16_t)P[2], (int16_t)P[3], (int16_t)P[4], (int16_t)P[5], (int16_t)P[6], (int16_t)P[7], (int16_t)P[8] };
    for (int i = 0; i < 12; i++) {
        calib[0x88 + 2 * i] = (uint8_t)c[i];
        calib[0x89 + 2 * i] = (uint8_t)((uint16_t)c[i] >> 8);
    }
    calib[0xa1] = (uint8_t)H1;
    calib[0xe1] = (uint8_t)((int)H2 & 0xff);
    calib[0xe2] = (uint8_t)((int)H2 >> 8);
    calib[0xe3] = (uint8_t)H3;
    calib[0xe4] = (uint8_t)((int)H4 >> 4);
    calib[0xe5] = (uint8_t)(((int)H4 & 0xf) | (((int)H5 & 0xf) << 4));
    calib[0xe6] = (uint8_t)((int)H5 >> 4);
    calib[0xe7] = (uint8_t)H6;
    uint32_t at = _invert(_bme_temperature, 0, temperature, 0xfffff, true);
    int t_fine = _bme_t_fine(at);
    uint32_t ah = humidity < 0 ? 0x8000 : _invert(_bme_humidity, t_fine, humidity, 0xffff, true);
    uint32_t ap = _invert(_bme_pressure, t_fine, pressure, 0xfffff, false);
    const uint8_t sample[8] = {
        (uint8_t)(ap >> 12), (uint8_t)(ap >> 4), (uint8_t)(ap << 4),
        (uint8_t)(at >> 12), (uint8_t)(at >> 4), (uint8_t)(at << 4),
        (uint8_t)(ah >> 8), (uint8_t)ah,
    };
    for (size_t i = 0; i < len; i++) {
        uint8_t reg = dev->pointer++;
        if (reg == 0xd0) {
            data[i] = chip_id;
        } else if ((reg >= 0x88 && reg <= 0xa1) || (reg >= 0xe1 && reg <= 0xe7)) {
            data[i] = calib[reg];
        } else if (reg >= 0xf7 && reg <= 0xfe) {
            data[i] = sample[reg - 0xf7];
        } else if (reg == 0xf3) {
            data[i] = 0;                // never measuring: a forced reading is ready at once
        } else {
            data[i] = dev->regs[reg];
        }
    }
}

static void _bme280_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    _bmx280_read(dev, data, len, 0x60, dev->value[0], dev->value[1], dev->value[2]);
}

static void _bmp280_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    _bmx280_read(dev, data, len, 0x58, dev->value[0], -1, dev->value[1]);
}

const i2c_virtual_model_t i2c_model_bme280 = {
    "bme280", 0x77, 3, {
        { "temp", "C", -40, 85, 22, 2 }, { "hum", "%", 0, 100, 45, 1 }, { "press", "hPa", 300, 1100, 1013.25, 2 }
    },
    i2c_model_plain_write, _bme280_read
};

const i2c_virtual_model_t i2c_model_bmp280 = {
    "bmp280", 0x77, 2, { { "temp", "C", -40, 85, 22, 2 }, { "press", "hPa", 300, 1100, 1013.25, 2 } },
    i2c_model_plain_write, _bmp280_read
};
