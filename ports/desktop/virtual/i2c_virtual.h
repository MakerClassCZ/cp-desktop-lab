// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "desktop_pins.h"
#include "vdisplay.h"

// The virtual board's I2C: a virtual bus on any pins with device models from the board file's
// [i2c] section (NAME = "MODEL [ADDRESS]"). Each model's values are sliders on the board panel.
// The bus runs on the VM thread; the panel sets values there too.

#define I2C_VIRTUAL_CHANNELS 4

typedef struct {
    const char *name;           // after the device's: ACCEL.x
    const char *unit;
    double min, max, initial;
    int decimals;
} i2c_virtual_channel_t;

struct i2c_virtual_device;

typedef struct {
    const char *name;
    uint8_t address;            // the default
    uint8_t channel_count;
    i2c_virtual_channel_t channels[I2C_VIRTUAL_CHANNELS];
    // A write: the register pointer, then data; a read: from the pointer on.
    void (*write)(struct i2c_virtual_device *dev, const uint8_t *data, size_t len);
    void (*read)(struct i2c_virtual_device *dev, uint8_t *data, size_t len);
    // A display sets itself up at the board file's size (0 when none was given); a driver's
    // scan of an empty address never attaches one.
    bool (*attach)(struct i2c_virtual_device *dev, unsigned width, unsigned height, const char **why);
} i2c_virtual_model_t;

typedef struct i2c_virtual_device {
    const i2c_virtual_model_t *model;
    char name[12];
    uint8_t address;
    uint8_t pointer;
    uint8_t regs[256];
    uint16_t words[256];        // a model with 16-bit registers
    double value[I2C_VIRTUAL_CHANNELS];
    long long clock_offset;     // a real-time clock: seconds from the computer's time
    int weekday_shift;          // and days from the weekday its date has
    // a display: what it shows, its memory a byte per eight pixels in a column, and the cursor
    vdisplay_t *display;
    uint8_t gram[128 * 64 / 8];
    uint8_t command, params_left, params[6];
    uint8_t mode, col0, col1, page0, page1, col, page;
    bool inverted;
    // a seesaw: the command a read answers, its GPIO (port A bits 0-31, B 32-63) and PWM outputs
    uint8_t seesaw_base, seesaw_function;
    uint64_t gpio_output, gpio_level, gpio_pull;
    uint16_t pwm[16], pwm_hz[16];
} i2c_virtual_device_t;

extern const desktop_i2c_ops_t i2c_virtual_ops;

// A board file line: false with *why set when the model is unknown or the address taken.
bool i2c_virtual_add(const char *name, const char *spec, const char **why);
// A channel's value from the panel or the key script: kept within its range and traced.
void i2c_virtual_set(i2c_virtual_device_t *dev, int channel, double value);
// The device at the address, or NULL.
i2c_virtual_device_t *i2c_virtual_at(uint16_t address);
// A driver looks for a device at an empty address: the first model whose own address it is
// appears there (named after it, BME280). False when none is, or the address is taken.
bool i2c_virtual_attach_default(uint8_t address);
