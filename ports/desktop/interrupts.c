// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// Interrupts on a host. A timer thread plays the interrupt controller: it runs the 1024 Hz tick
// and the one-shot alarms, and "disabling interrupts" holds the lock the thread takes around
// each of them. The VM idles on a condition variable that the thread, the console reader and the
// drive watcher signal, as an interrupt wakes an MCU. All of it is pthreads, which MinGW provides
// on Windows too.

#ifdef _WIN32
#include <windows.h>
#endif
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "supervisor/background_callback.h"
#include "supervisor/port.h"
#include "supervisor/shared/safe_mode.h"
#include "supervisor/shared/tick.h"
#include "supervisor/workflow.h"

#include "desktop_port.h"

#define NS_PER_S 1000000000ULL
#define TICK_NS (NS_PER_S / 1024)

// Held while interrupts are disabled, and by the timer thread around each interrupt.
static pthread_mutex_t _irq_lock;

// The alarms, in CLOCK_MONOTONIC nanoseconds; 0 = not armed.
static pthread_mutex_t _timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t _timer_cond = PTHREAD_COND_INITIALIZER;
static bool _tick_enabled;
static uint64_t _next_tick_ns;
static uint64_t _wake_ns;
static uint64_t _poll_ns;

// Set by whoever wakes the VM, cleared by the VM as it leaves idle.
static pthread_mutex_t _wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t _wake_cond = PTHREAD_COND_INITIALIZER;
static bool _woken;

static uint64_t _now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * NS_PER_S + now.tv_nsec;
}

// Fast time: the sleep skipped so far, and the deadline the VM would idle until in the program's
// nanoseconds (0 = none), which only the VM thread touches.
static uint64_t _skipped_ns;
static uint64_t _fast_deadline_ns;

uint64_t desktop_skipped_ns(void) {
    return __atomic_load_n(&_skipped_ns, __ATOMIC_RELAXED);
}

uint64_t desktop_program_ns(void) {
    return _now_ns() + desktop_skipped_ns();
}

// The tick's work, under the lock as an MCU's tick interrupt.
static void _tick(void) {
    desktop_irq_disable();
    supervisor_tick();
    desktop_irq_enable();
}

// On the VM thread. The background tasks run once for the time skipped, as its ticks would.
void desktop_skip_ns(uint64_t ns) {
    __atomic_add_fetch(&_skipped_ns, ns, __ATOMIC_RELAXED);
    _tick();
}

uint64_t desktop_host_ms(void) {
    return _now_ns() / 1000000;
}

void desktop_host_sleep_ms(unsigned ms) {
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0) {
    }
}

int64_t desktop_program_time(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)(((uint64_t)t.tv_sec * NS_PER_S + (uint64_t)t.tv_nsec + desktop_skipped_ns()) / NS_PER_S);
}

// The caller's loop waits out the last ms itself and so ends on time: a display's frame pacing
// takes a late end for a missed frame.
void desktop_fast_wait_ms(int timeout_ms) {
    if (desktop_options.fast_time && timeout_ms > 1) {
        desktop_skip_ns((uint64_t)(timeout_ms - 1) * 1000000ULL);
    }
}

// pthread_cond_timedwait takes a CLOCK_REALTIME deadline.
static struct timespec _realtime_after(uint64_t ns) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    uint64_t nsec = (uint64_t)t.tv_nsec + ns;
    t.tv_sec += nsec / NS_PER_S;
    t.tv_nsec = nsec % NS_PER_S;
    return t;
}

// Nested disable/enable pairs work as on an MCU, and the lock exists before main() runs.
__attribute__((constructor)) static void _init_irq_lock(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&_irq_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

void desktop_irq_disable(void) {
    pthread_mutex_lock(&_irq_lock);
}

void desktop_irq_enable(void) {
    if (pthread_mutex_unlock(&_irq_lock) != 0) {
        reset_into_safe_mode(SAFE_MODE_INTERRUPT_ERROR);   // enabled more often than disabled
    }
}

void desktop_wake_vm(void) {
    pthread_mutex_lock(&_wake_lock);
    _woken = true;
    pthread_cond_signal(&_wake_cond);
    pthread_mutex_unlock(&_wake_lock);
}

void desktop_wait_for_interrupt(int timeout_ms) {
    pthread_mutex_lock(&_wake_lock);
    if (!_woken && !background_callback_pending()) {
        if (timeout_ms < 0) {
            pthread_cond_wait(&_wake_cond, &_wake_lock);
        } else {
            struct timespec deadline = _realtime_after((uint64_t)timeout_ms * 1000000ULL);
            pthread_cond_timedwait(&_wake_cond, &_wake_lock, &deadline);
        }
    }
    _woken = false;
    pthread_mutex_unlock(&_wake_lock);
}

void port_idle_until_interrupt(void) {
    uint64_t deadline = _fast_deadline_ns;
    _fast_deadline_ns = 0;
    if (deadline != 0) {
        uint64_t now = desktop_program_ns();
        if (deadline > now) {
            desktop_skip_ns(deadline - now);
        }
        return;
    }
    desktop_wait_for_interrupt(-1);
}

// The next alarm among those armed, or 0; due alarms are taken.
static uint64_t _due(uint64_t *slot, uint64_t now, bool *fired, uint64_t next) {
    if (*slot == 0) {
        return next;
    }
    if (*slot <= now) {
        *fired = true;
        *slot = 0;
        return next;
    }
    return next == 0 || *slot < next ? *slot : next;
}

static void *_timer_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&_timer_lock);
    for (;;) {
        uint64_t now = _now_ns();
        uint64_t next = 0;
        bool tick = false;
        bool wake = false;
        bool poll = false;
        if (_tick_enabled) {
            if (_next_tick_ns <= now) {
                tick = true;
                _next_tick_ns += TICK_NS;
                if (_next_tick_ns <= now) {
                    _next_tick_ns = now + TICK_NS;      // after a stall, skip the missed ticks
                }
            }
            next = _next_tick_ns;
        }
        next = _due(&_wake_ns, now, &wake, next);
        next = _due(&_poll_ns, now, &poll, next);
        if (tick || wake || poll) {
            pthread_mutex_unlock(&_timer_lock);
            if (tick) {
                _tick();
            }
            #if CIRCUITPY_SOCKETPOOL
            if (poll) {
                desktop_irq_disable();
                supervisor_workflow_request_background();
                desktop_irq_enable();
            }
            #endif
            desktop_wake_vm();
            pthread_mutex_lock(&_timer_lock);
            continue;
        }
        if (next == 0) {
            pthread_cond_wait(&_timer_cond, &_timer_lock);
        } else {
            struct timespec deadline = _realtime_after(next - now);
            pthread_cond_timedwait(&_timer_cond, &_timer_lock, &deadline);
        }
    }
    return NULL;
}

bool desktop_interrupts_init(void) {
    #ifdef _WIN32
    timeBeginPeriod(1);     // the scheduler's default 15.6 ms would quantise the tick
    #endif
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int err = pthread_create(&thread, &attr, _timer_main, NULL);
    pthread_attr_destroy(&attr);
    return err == 0;
}

static void _arm(uint64_t *slot, uint64_t ns) {
    pthread_mutex_lock(&_timer_lock);
    *slot = _now_ns() + (ns > 0 ? ns : 1);
    pthread_cond_signal(&_timer_cond);
    pthread_mutex_unlock(&_timer_lock);
}

void port_enable_tick(void) {
    pthread_mutex_lock(&_timer_lock);
    _tick_enabled = true;
    _next_tick_ns = _now_ns() + TICK_NS;
    pthread_cond_signal(&_timer_cond);
    pthread_mutex_unlock(&_timer_lock);
}

void port_disable_tick(void) {
    pthread_mutex_lock(&_timer_lock);
    _tick_enabled = false;
    pthread_mutex_unlock(&_timer_lock);
}

void port_interrupt_after_ticks(uint32_t ticks) {
    // 0 ticks is main.c's wait after code.py with no ePaper to refresh: no time bound, the idle
    // waits for input or a file change, as an alarm due at once would spin a core.
    if (ticks == 0) {
        pthread_mutex_lock(&_timer_lock);
        _wake_ns = 0;
        pthread_mutex_unlock(&_timer_lock);
        _fast_deadline_ns = 0;
        return;
    }
    uint64_t ns = (uint64_t)ticks * NS_PER_S / 1024;
    // In fast time the idle jumps to the deadline instead; exactly, as a display's frame pacing
    // takes a late wake for a missed frame.
    if (desktop_options.fast_time) {
        _fast_deadline_ns = desktop_program_ns() + ns;
        return;
    }
    _arm(&_wake_ns, ns);
}

void desktop_request_workflow_poll(uint32_t ms) {
    _arm(&_poll_ns, (uint64_t)ms * 1000000ULL);
}
