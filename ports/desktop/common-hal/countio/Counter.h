// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// A counter on a virtual pin: it counts the edges of the level the panel puts there (a button
// pressed, a gamepad's), and the panel line "count PIN N" adds N edges at once.
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *pin;
    mp_int_t count;
    uint8_t edge;           // countio_edge_t
    bool level;
} countio_counter_obj_t;

// The pin's level is now level: the counters there count the edge.
void countio_pin_level(uint8_t pin, bool level);
// "count PIN N": N more edges for the counters on the pin; false when there is none.
bool countio_add(uint8_t pin, mp_int_t n);
// After a VM: no counter is left.
void countio_reset(void);
