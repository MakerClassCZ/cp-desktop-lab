// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// A pulse train on a virtual pin: nothing listens, so the bus trace records it and send() takes
// as long as the pulses would.
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *pin;
    uint32_t frequency;
} pulseio_pulseout_obj_t;
