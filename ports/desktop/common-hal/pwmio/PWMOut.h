// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// pwmio on the virtual board: a lamp (or a servo) of the board panel (parts.c).
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *pin;
    void *part;
    uint16_t duty;
    uint32_t frequency;
    bool variable_frequency;
} pwmio_pwmout_obj_t;
