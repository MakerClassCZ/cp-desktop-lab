// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// touchio on the virtual board: a pad of the board panel (parts.c), touched while held.
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *pin;
    void *part;
    uint16_t threshold;
} touchio_touchin_obj_t;
