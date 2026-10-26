// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/audioio/AudioOut.h"
#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// I2SOut plays through the computer's sound output, as audioio.AudioOut does (it is one inside),
// with a speaker row on the board panel.
typedef struct {
    mp_obj_base_t base;
    audioio_audioout_obj_t out;
    const mcu_pin_obj_t *bit_clock;
    const mcu_pin_obj_t *word_select;
    const mcu_pin_obj_t *data;
    void *part;
} audiobusio_i2sout_obj_t;
