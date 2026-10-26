// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// A pulse reader on a virtual pin: the board panel's IR receiver on that pin hands it the pulses
// of a remote's code (desktop_pulsein_push); they queue up to maxlen, the oldest dropped.
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *pin;
    void *part;
    uint16_t *buffer;
    uint16_t maxlen;
    uint16_t start;
    uint16_t len;
    bool idle_state;
    bool paused;
} pulseio_pulsein_obj_t;

// Pulses in microseconds, the first one away from idle.
void desktop_pulsein_push(pulseio_pulsein_obj_t *self, const uint16_t *pulses, size_t count);
