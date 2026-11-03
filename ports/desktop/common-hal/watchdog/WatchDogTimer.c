// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The watchdog on the program's clock (its sleeps skipped by --fast-time count, as on a board):
// RAISE raises watchdog.WatchDogTimeout in the program, RESET resets the board as
// microcontroller.reset() does.

#include "py/runtime.h"

#include "shared-bindings/microcontroller/__init__.h"
#include "shared-bindings/watchdog/__init__.h"

#include "common-hal/watchdog/WatchDogTimer.h"
#include "desktop_port.h"

static void _arm(watchdog_watchdogtimer_obj_t *self) {
    self->deadline_ns = desktop_program_ns() + (uint64_t)(self->timeout * 1e9);
}

void common_hal_watchdog_feed(watchdog_watchdogtimer_obj_t *self) {
    _arm(self);
}

void common_hal_watchdog_reset(watchdog_watchdogtimer_obj_t *self) {
    self->mode = WATCHDOGMODE_NONE;
}

mp_float_t common_hal_watchdog_get_timeout(watchdog_watchdogtimer_obj_t *self) {
    return self->timeout;
}

void common_hal_watchdog_set_timeout(watchdog_watchdogtimer_obj_t *self, mp_float_t new_timeout) {
    if (new_timeout <= 0) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be > %d"), MP_QSTR_timeout, 0);
    }
    self->timeout = new_timeout;
    _arm(self);
}

watchdog_watchdogmode_t common_hal_watchdog_get_mode(watchdog_watchdogtimer_obj_t *self) {
    return self->mode;
}

void common_hal_watchdog_set_mode(watchdog_watchdogtimer_obj_t *self, watchdog_watchdogmode_t new_mode) {
    if (new_mode != WATCHDOGMODE_NONE && self->mode == WATCHDOGMODE_NONE) {
        _arm(self);
    }
    self->mode = new_mode;
}

void desktop_watchdog_check(void) {
    watchdog_watchdogtimer_obj_t *self = &common_hal_mcu_watchdogtimer_obj;
    if (self->mode == WATCHDOGMODE_NONE || desktop_program_ns() < self->deadline_ns) {
        return;
    }
    if (self->mode == WATCHDOGMODE_RESET) {
        common_hal_mcu_reset();
    }
    // RAISE: once, as a board's watchdog stops when it fires.
    self->mode = WATCHDOGMODE_NONE;
    mp_obj_exception_clear_traceback(MP_OBJ_FROM_PTR(&mp_watchdog_timeout_exception));
    MP_STATE_THREAD(mp_pending_exception) = &mp_watchdog_timeout_exception;
    #if MICROPY_ENABLE_SCHEDULER
    if (MP_STATE_VM(sched_state) == MP_SCHED_IDLE) {
        MP_STATE_VM(sched_state) = MP_SCHED_PENDING;
    }
    #endif
}
