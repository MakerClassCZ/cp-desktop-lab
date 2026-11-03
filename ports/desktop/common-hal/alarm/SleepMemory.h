// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "py/obj.h"

// alarm.sleep_memory: kept through a deep sleep (the process's restart), lost when it ends.
#define DESKTOP_SLEEP_MEMORY_SIZE 4096

typedef struct {
    mp_obj_base_t base;
} alarm_sleep_memory_obj_t;

extern uint8_t desktop_sleep_memory[DESKTOP_SLEEP_MEMORY_SIZE];
