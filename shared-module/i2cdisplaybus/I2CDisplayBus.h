// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/busio/I2C.h"
#include "common-hal/digitalio/DigitalInOut.h"

typedef struct {
    mp_obj_base_t base;
    busio_i2c_obj_t *bus;
    busio_i2c_obj_t inline_bus;
    digitalio_digitalinout_obj_t reset;
    uint16_t address;
} i2cdisplaybus_i2cdisplaybus_obj_t;

// A port can put a model of the display at its address (one it emulates); weak, does nothing by default.
void i2cdisplaybus_port_attach(busio_i2c_obj_t *i2c, uint16_t device_address);
