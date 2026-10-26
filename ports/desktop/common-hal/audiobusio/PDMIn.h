// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// PDMIn records the board panel's microphone: a generator (a tone or noise as loud as its
// slider) or, with "mic", the computer's own microphone.
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *clock;
    const mcu_pin_obj_t *data;
    uint32_t sample_rate;
    uint8_t bit_depth;
    void *part;
    uint32_t device;            // SDL_AudioDeviceID of the computer's microphone, or 0
} audiobusio_pdmin_obj_t;
