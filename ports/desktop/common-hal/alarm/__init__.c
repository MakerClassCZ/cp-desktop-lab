// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// alarm on the desktop. A light sleep waits in the VM for a time or a pin's level. A deep sleep
// is a board's: the program ends, the process waits for an alarm (or a key on the console, the
// reset button's stand-in) and starts again from boot.py, alarm.wake_alarm telling what woke it
// and alarm.sleep_memory kept; both ride the restart in the environment, as the safe mode word
// does.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/gc.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/runtime/interrupt_char.h"

#include "shared-bindings/alarm/SleepMemory.h"
#include "shared-bindings/alarm/__init__.h"
#include "shared-bindings/alarm/pin/PinAlarm.h"
#include "shared-bindings/alarm/time/TimeAlarm.h"
#include "shared-bindings/time/__init__.h"
#include "supervisor/port.h"
#include "supervisor/shared/serial.h"

#include "desktop_pins.h"
#include "desktop_port.h"
#include "platform/platform.h"

#define WAKE_ENV "CIRCUITPY_DESKTOP_WAKE"
#define MEMORY_ENV "CIRCUITPY_DESKTOP_SLEEP_MEMORY"
#define WATCH_MAX 8

const alarm_sleep_memory_obj_t alarm_sleep_memory_obj = {
    .base = {
        .type = &alarm_sleep_memory_type,
    },
};

alarm_wake_alarm_union_t alarm_wake_alarm;

// An alarm as the sleep watches it: kept outside the heap, which a deep sleep outlives.
typedef struct {
    bool is_time;
    mp_float_t when;            // a time alarm: time.monotonic()'s value
    uint8_t pin;                // a pin alarm
    bool value, edge, pull, last;
} watch_t;

static watch_t _deep[WATCH_MAX];
static size_t _deep_count;

// What woke the board, from the restart: 0 nothing, 't' a time alarm, 'p' a pin alarm.
static char _woke;
static uint8_t _woke_pin;
static bool _woke_value;
bool desktop_alarm_woke;    // the reset was a deep sleep's (the processor's reset reason)

static mp_float_t _now(void) {
    return (mp_float_t)common_hal_time_monotonic_ms() / 1000;
}

static bool _level(const watch_t *w) {
    bool level = !w->value;
    if (!desktop_pin_backend->read(w->pin, &level)) {
        return !w->value;
    }
    return level;
}

static void _arm(watch_t *w) {
    if (!w->is_time) {
        // Pulled away from the value it waits for, as a board's pin alarm with pull=True.
        desktop_pin_backend->configure(w->pin, false, false,
            !w->pull ? PULL_NONE : w->value ? PULL_DOWN : PULL_UP);
        w->last = _level(w);
    }
}

static size_t _watch(size_t n_alarms, const mp_obj_t *alarms, watch_t *out) {
    size_t n = 0;
    for (size_t i = 0; i < n_alarms && n < WATCH_MAX; i++) {
        watch_t w = { 0 };
        if (mp_obj_is_type(alarms[i], &alarm_time_timealarm_type)) {
            alarm_time_timealarm_obj_t *a = MP_OBJ_TO_PTR(alarms[i]);
            w.is_time = true;
            w.when = a->monotonic_time;
        } else if (mp_obj_is_type(alarms[i], &alarm_pin_pinalarm_type)) {
            alarm_pin_pinalarm_obj_t *a = MP_OBJ_TO_PTR(alarms[i]);
            w.pin = a->pin->number;
            w.value = a->value;
            w.edge = a->edge;
            w.pull = a->pull;
        } else {
            mp_raise_TypeError_varg(MP_ERROR_TEXT("%q alarm not supported on this board"), mp_obj_get_type_qstr(alarms[i]));
        }
        out[n++] = w;
    }
    for (size_t i = 0; i < n; i++) {
        _arm(&out[i]);
    }
    return n;
}

static bool _fired(watch_t *w) {
    if (w->is_time) {
        return _now() >= w->when;
    }
    bool level = _level(w);
    bool fired = w->edge ? (level != w->last && level == w->value) : level == w->value;
    w->last = level;
    return fired;
}

// Until an alarm fires: its index; -1 when stop() says so first.
static int _wait(watch_t *watch, size_t n, bool (*stop)(void)) {
    for (;;) {
        mp_float_t earliest = 0;
        bool timed = false, pins = false;
        for (size_t i = 0; i < n; i++) {
            if (_fired(&watch[i])) {
                return (int)i;
            }
            if (watch[i].is_time) {
                earliest = !timed || watch[i].when < earliest ? watch[i].when : earliest;
                timed = true;
            } else {
                pins = true;
            }
        }
        if (stop()) {
            return -1;
        }
        RUN_BACKGROUND_TASKS;
        // --fast-time: a sleep with nothing but time to wait for is over at once.
        if (desktop_options.fast_time && timed && !pins) {
            mp_float_t ahead = earliest - _now();
            if (ahead > 0) {
                desktop_skip_ns((uint64_t)(ahead * 1e9) + 1000000);
            }
            continue;
        }
        desktop_wait_for_interrupt(10);
    }
}

static bool _interrupted(void) {
    return mp_hal_is_interrupted();
}

void alarm_reset(void) {
    _woke = 0;
}

bool common_hal_alarm_woken_from_sleep(void) {
    return _woke != 0;
}

mp_obj_t common_hal_alarm_record_wake_alarm(void) {
    if (_woke == 't') {
        alarm_wake_alarm.time_alarm.base.type = &alarm_time_timealarm_type;
        alarm_wake_alarm.time_alarm.monotonic_time = _now();
        return MP_OBJ_FROM_PTR(&alarm_wake_alarm.time_alarm);
    }
    if (_woke == 'p') {
        alarm_wake_alarm.pin_alarm.base.type = &alarm_pin_pinalarm_type;
        alarm_wake_alarm.pin_alarm.pin = desktop_pin_obj(_woke_pin);
        alarm_wake_alarm.pin_alarm.value = _woke_value;
        alarm_wake_alarm.pin_alarm.edge = false;
        alarm_wake_alarm.pin_alarm.pull = false;
        return MP_OBJ_FROM_PTR(&alarm_wake_alarm.pin_alarm);
    }
    return mp_const_none;
}

mp_obj_t common_hal_alarm_light_sleep_until_alarms(size_t n_alarms, const mp_obj_t *alarms) {
    watch_t watch[WATCH_MAX];
    size_t n = _watch(n_alarms, alarms, watch);
    int fired = _wait(watch, n, _interrupted);
    if (fired < 0) {
        return mp_const_none;   // Ctrl-C: the exception comes next
    }
    // The alarm given, as alarm.wake_alarm is too.
    size_t k = 0;
    for (size_t i = 0; i < n_alarms; i++) {
        if (mp_obj_is_type(alarms[i], &alarm_time_timealarm_type) || mp_obj_is_type(alarms[i], &alarm_pin_pinalarm_type)) {
            if (k++ == (size_t)fired) {
                shared_alarm_save_wake_alarm(alarms[i]);
                return alarms[i];
            }
        }
    }
    return mp_const_none;
}

void common_hal_alarm_set_deep_sleep_alarms(size_t n_alarms, const mp_obj_t *alarms, size_t n_dios, digitalio_digitalinout_obj_t **preserve_dios) {
    _deep_count = _watch(n_alarms, alarms, _deep);
}

static bool _key_pressed(void) {
    return serial_bytes_available() > 0;
}

void MP_NORETURN common_hal_alarm_enter_deep_sleep(void) {
    // The VM's pins are reset by now: the pin alarms set up again.
    for (size_t i = 0; i < _deep_count; i++) {
        _arm(&_deep[i]);
    }
    int fired = _wait(_deep, _deep_count, _key_pressed);
    char wake[24] = "";
    if (fired >= 0) {
        const watch_t *w = &_deep[fired];
        if (w->is_time) {
            snprintf(wake, sizeof(wake), "t");
        } else {
            snprintf(wake, sizeof(wake), "p%u:%d", (unsigned)w->pin, w->value ? 1 : 0);
        }
    }
    platform_set_env(WAKE_ENV, fired >= 0 ? wake : NULL);
    static char hex[2 * DESKTOP_SLEEP_MEMORY_SIZE + 1];
    for (size_t i = 0; i < DESKTOP_SLEEP_MEMORY_SIZE; i++) {
        snprintf(hex + 2 * i, 3, "%02x", desktop_sleep_memory[i]);
    }
    platform_set_env(MEMORY_ENV, hex);
    reset_cpu();
}

void desktop_alarm_init(void) {
    const char *wake = getenv(WAKE_ENV);
    if (wake != NULL) {
        unsigned pin;
        int value;
        if (wake[0] == 't') {
            _woke = 't';
        } else if (sscanf(wake, "p%u:%d", &pin, &value) == 2) {
            _woke = 'p';
            _woke_pin = (uint8_t)pin;
            _woke_value = value != 0;
        }
        desktop_alarm_woke = _woke != 0;
        platform_set_env(WAKE_ENV, NULL);
    }
    const char *memory = getenv(MEMORY_ENV);
    if (memory != NULL) {
        for (size_t i = 0; i < DESKTOP_SLEEP_MEMORY_SIZE && memory[2 * i] && memory[2 * i + 1]; i++) {
            char hex2[3] = { memory[2 * i], memory[2 * i + 1], '\0' };
            desktop_sleep_memory[i] = (uint8_t)strtoul(hex2, NULL, 16);
        }
        platform_set_env(MEMORY_ENV, NULL);
    }
}

void common_hal_alarm_gc_collect(void) {
    gc_collect_ptr(shared_alarm_get_wake_alarm());
}
