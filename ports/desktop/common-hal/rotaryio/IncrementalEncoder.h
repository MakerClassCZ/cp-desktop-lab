// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// An encoder on virtual pins: the board panel's part on pin_a counts the quadrature steps the user
// turned; the position is those since the last set, over the divisor.
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *pin_a;
    const mcu_pin_obj_t *pin_b;
    void *part;
    int32_t base_steps;     // the part's steps when the position was last set
    mp_int_t base_position;
    mp_int_t divisor;
} rotaryio_incrementalencoder_obj_t;
