// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "py/obj.h"

#include "shared-module/watchdog/__init__.h"

#include "shared-bindings/watchdog/WatchDogMode.h"
#include "shared-bindings/watchdog/WatchDogTimer.h"

struct _watchdog_watchdogtimer_obj_t {
    mp_obj_base_t base;
    mp_float_t timeout;
    watchdog_watchdogmode_t mode;
    uint64_t deadline_ns;   // on the program's clock: when an unfed watchdog fires
};

// The background's look at the watchdog: past its deadline it raises or resets.
void desktop_watchdog_check(void);
