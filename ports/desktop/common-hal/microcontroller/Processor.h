// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#define COMMON_HAL_MCU_PROCESSOR_UID_LENGTH 16

#include "py/obj.h"

typedef struct {
    mp_obj_base_t base;
} mcu_processor_obj_t;
