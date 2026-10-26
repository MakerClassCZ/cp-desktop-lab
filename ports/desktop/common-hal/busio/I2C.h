// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// I2C through the FT232H's MPSSE engine: SCL = D0, SDA = D1 out, read back on D2.
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *scl;
    const mcu_pin_obj_t *sda;
    bool has_lock;
    uint32_t frequency;
} busio_i2c_obj_t;
