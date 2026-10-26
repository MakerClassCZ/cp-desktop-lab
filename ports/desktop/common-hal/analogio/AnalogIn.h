// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// The MCP2221's ADC: channels 1-3 on G1-G3 (pins 3-5), 10 bits against VDD. On the virtual
// board a slider of the board panel (parts.c).
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *pin;
    uint8_t channel;
    void *part;                 // the panel's part, on the virtual board
} analogio_analogin_obj_t;
