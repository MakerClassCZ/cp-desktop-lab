// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "i2c_virtual.h"

// The virtual I2C bus's device models, one file each in this directory, listed in i2c_virtual.c.

// A register pointer, then the bytes written from it on.
void i2c_model_plain_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len);
// The same for ST's sensors: the pointer's bit 7 asks for auto-increment, and reset_bit of
// reset_reg clears itself.
void i2c_model_st_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len, uint8_t reset_reg, uint8_t reset_bit);

extern const i2c_virtual_model_t i2c_model_lis3dh;
extern const i2c_virtual_model_t i2c_model_bme280;
extern const i2c_virtual_model_t i2c_model_bmp280;
extern const i2c_virtual_model_t i2c_model_lsm6ds33;
extern const i2c_virtual_model_t i2c_model_lis3mdl;
extern const i2c_virtual_model_t i2c_model_apds9960;
extern const i2c_virtual_model_t i2c_model_crickit;
extern const i2c_virtual_model_t i2c_model_sht31d;
extern const i2c_virtual_model_t i2c_model_mcp9808;
extern const i2c_virtual_model_t i2c_model_ds3231;
extern const i2c_virtual_model_t i2c_model_lc709203f;
extern const i2c_virtual_model_t i2c_model_ssd1306;
