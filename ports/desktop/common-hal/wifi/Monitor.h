// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// A computer's Wi-Fi card is not the program's to listen with: wifi.Monitor raises.
typedef struct {
    mp_obj_base_t base;
} wifi_monitor_obj_t;
