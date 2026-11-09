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
// A seesaw board: its chip and product, and which of the device's channels its touch pads,
// thermometer, encoder and button (pressed at 1, pulling its pin low) read; -1 for none.
typedef struct {
    uint8_t hw_id;
    uint16_t pid;
    const char *const *pwm;     // its PWM outputs' names, in the timer's order
    uint8_t pwm_count;
    int8_t touch_channel[4];
    int8_t temp_channel;
    int8_t encoder_channel;
    uint8_t button_pin;
    int8_t button_channel;
} seesaw_product_t;
void i2c_model_seesaw_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len, const seesaw_product_t *p);
void i2c_model_seesaw_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len, const seesaw_product_t *p);
// A capacitive touch controller's point registers 0x00-0x0f (ft6206.c), from channels x, y, touched.
uint8_t i2c_model_touch_reg(i2c_virtual_device_t *dev, uint8_t reg);
// A Sensirion sensor's word as it reads: most significant byte first, then its CRC.
void i2c_model_sensirion_word(uint8_t *out, uint16_t word);

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
extern const i2c_virtual_model_t i2c_model_pca9685;
extern const i2c_virtual_model_t i2c_model_drv2605;
extern const i2c_virtual_model_t i2c_model_max17048;
extern const i2c_virtual_model_t i2c_model_aht20;
extern const i2c_virtual_model_t i2c_model_scd40;
extern const i2c_virtual_model_t i2c_model_scd41;
extern const i2c_virtual_model_t i2c_model_sht4x;
extern const i2c_virtual_model_t i2c_model_rotary;
extern const i2c_virtual_model_t i2c_model_soil;
extern const i2c_virtual_model_t i2c_model_ht16k33;
extern const i2c_virtual_model_t i2c_model_is31fl3741;
extern const i2c_virtual_model_t i2c_model_tsc2007;
extern const i2c_virtual_model_t i2c_model_ft6206;
extern const i2c_virtual_model_t i2c_model_cst816;
