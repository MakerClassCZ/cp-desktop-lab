// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// SCD40 and SCD41: 16-bit commands, a setting's value after it as a word with its CRC; a read
// answers the command written last. A measurement is ready while the periodic one runs, and
// after a single shot (the SCD41's) until it is read.

enum { IDLE, PERIODIC, SINGLE_SHOT };

// The settings each "set" command writes and its "get" command reads, and their defaults.
static const struct {
    uint16_t set, get, initial;
} _settings[] = {
    { 0x241d, 0x2318, 1498 },   // temperature offset: 4 C, in 175 / 65536 C
    { 0x2427, 0x2322, 0 },      // altitude, m
    { 0xe000, 0xe000, 1013 },   // ambient pressure, hPa
    { 0x2416, 0x2313, 1 },      // automatic self-calibration on
    { 0x243a, 0x233f, 400 },    // its target, ppm
    { 0x2445, 0x2340, 44 },     // its initial period, h
    { 0x244e, 0x234b, 156 },    // its standard period, h
};
#define SETTINGS (sizeof(_settings) / sizeof(_settings[0]))
// Where a setting is kept: words 0 and 1 are the command and a recalibration's correction.
#define KEPT_AT(i) (0x80 + (i))

static void _defaults(i2c_virtual_device_t *dev) {
    for (size_t i = 0; i < SETTINGS; i++) {
        dev->words[KEPT_AT(i)] = _settings[i].initial;
    }
}

static void _scd4x_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    if (len < 2) {
        return;
    }
    uint16_t command = (uint16_t)(data[0] << 8 | data[1]);
    bool scd41 = dev->model == &i2c_model_scd41;
    if (!dev->regs[0]) {
        dev->regs[0] = 1;
        _defaults(dev);
    }
    dev->words[0] = command;
    if (len >= 5) {
        uint16_t value = (uint16_t)(data[2] << 8 | data[3]);
        for (size_t i = 0; i < SETTINGS; i++) {
            if (command == _settings[i].set) {
                dev->words[KEPT_AT(i)] = value;
            }
        }
        if (command == 0x362f) {            // forced recalibration: the correction made
            dev->words[1] = (uint16_t)(0x8000 + value - lround(dev->value[0]));
        }
    }
    switch (command) {
        case 0x21b1:                        // start periodic measurement
        case 0x21ac:                        // the low power one
            dev->mode = PERIODIC;
            break;
        case 0x3f86:                        // stop it
        case 0x36e0:                        // power down
            dev->mode = IDLE;
            break;
        case 0x219d:                        // a single shot, the SCD41's
        case 0x2196:                        // of temperature and humidity only
            if (scd41) {
                dev->mode = SINGLE_SHOT;
            } else {
                dev->nack = true;
            }
            break;
        case 0x3632:                        // factory reset
            _defaults(dev);
            dev->mode = IDLE;
            break;
        default:
            break;
    }
}

static void _scd4x_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    uint16_t words[3] = { 0 };
    uint16_t command = dev->words[0];
    switch (command) {
        case 0xec05:                        // read measurement: CO2, temperature, humidity
            words[0] = (uint16_t)lround(dev->value[0]);
            words[1] = (uint16_t)lround((dev->value[1] + 45) * 65535 / 175);
            words[2] = (uint16_t)lround(dev->value[2] * 65535 / 100);
            if (dev->mode == SINGLE_SHOT) {
                dev->mode = IDLE;
            }
            break;
        case 0xe4b8:                        // data ready: any of the low 11 bits set
            words[0] = dev->mode != IDLE ? 0x8006 : 0x8000;
            break;
        case 0x3682:                        // the serial number
            words[0] = 0x1234;
            words[1] = 0x5678;
            words[2] = 0x9abc;
            break;
        case 0x202f:                        // the variant, in bits 15-12: 0 SCD40, 1 SCD41
            words[0] = dev->model == &i2c_model_scd41 ? 0x1000 : 0x0000;
            break;
        case 0x362f:
            words[0] = dev->words[1];
            break;
        case 0x3639:                        // self test: 0, passed
            break;
        default:
            for (size_t i = 0; i < SETTINGS; i++) {
                if (command == _settings[i].get) {
                    words[0] = dev->words[KEPT_AT(i)];
                }
            }
    }
    uint8_t frame[9];
    for (int i = 0; i < 3; i++) {
        i2c_model_sensirion_word(frame + 3 * i, words[i]);
    }
    for (size_t i = 0; i < len; i++) {
        data[i] = i < sizeof(frame) ? frame[i] : 0;
    }
}

#define SCD4X_CHANNELS { { "co2", "ppm", 400, 5000, 600, 0 }, { "temp", "C", -10, 60, 22, 2 }, { "hum", "%", 0, 100, 45, 1 } }

const i2c_virtual_model_t i2c_model_scd41 = {
    "scd41", 0x62, 3, SCD4X_CHANNELS, _scd4x_write, _scd4x_read, .driver = "adafruit_scd4x"
};

const i2c_virtual_model_t i2c_model_scd40 = {
    "scd40", 0x62, 3, SCD4X_CHANNELS, _scd4x_write, _scd4x_read, .driver = "adafruit_scd4x"
};
