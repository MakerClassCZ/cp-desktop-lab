// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// A scan finds one network: the one the computer is on.
typedef struct {
    mp_obj_base_t base;
    mp_obj_t network;           // MP_OBJ_NULL once given out
} wifi_scannednetworks_obj_t;
