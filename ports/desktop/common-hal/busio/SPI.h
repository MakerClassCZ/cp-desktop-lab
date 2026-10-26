// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "py/obj.h"

// busio.SPI over the pin backend (common-hal/busio/SPI.c): the pins and the settings.
typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *clock;
    const mcu_pin_obj_t *mosi;
    const mcu_pin_obj_t *miso;
    bool has_lock;
    uint32_t frequency;
    uint8_t polarity;
    uint8_t phase;
} busio_spi_obj_t;
