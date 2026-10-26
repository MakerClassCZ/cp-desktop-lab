// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// The MCP2221's one DAC, on G2 or G3 (pins 4-5), 5 bits against VDD. On the virtual board a bar
// of the board panel (parts.c).
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *pin;
    void *part;                 // the panel's part, on the virtual board
} analogio_analogout_obj_t;
