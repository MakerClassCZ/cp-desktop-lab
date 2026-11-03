// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "common-hal/microcontroller/Pin.h"

// shared-bindings/alarm includes it always; the desktop has no touch alarms (CIRCUITPY_ALARM_TOUCH 0).
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *pin;
} alarm_touch_touchalarm_obj_t;
