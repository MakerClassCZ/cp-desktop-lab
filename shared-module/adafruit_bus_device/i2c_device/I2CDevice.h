// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "common-hal/busio/I2C.h"

typedef struct {
    mp_obj_base_t base;
    mp_obj_t *i2c;
    uint8_t device_address;
} adafruit_bus_device_i2cdevice_obj_t;

// A port can put a model of the device a driver looks for at its address (one it emulates); weak, does nothing by default.
void i2cdevice_port_attach(mp_obj_t i2c, uint8_t device_address);
