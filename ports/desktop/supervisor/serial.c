// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The serial console is the process's stdin and stdout. A reader thread plays the part of the
// UART receive interrupt: it buffers input, raises KeyboardInterrupt on the interrupt character
// and wakes the VM. The terminal is put in raw mode, so Ctrl-C arrives as a byte like on a UART.

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/runtime/interrupt_char.h"
#include "supervisor/shared/serial.h"

#include "control_api.h"
#include "desktop_port.h"
#include "platform.h"

#define RX_SIZE 1024
#define CTRL_BACKSLASH 0x1c
// Empty polls in a row before a poll sleeps until input or the next interrupt.
#define IDLE_POLLS 64

static uint8_t _rx[RX_SIZE];
static uint32_t _rx_head;           // written by the input threads, under _rx_lock
static uint32_t _rx_tail;           // written by the VM thread
static pthread_mutex_t _rx_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t _empty_polls;
static bool _tty;

// Input from the reader thread or the control API: the power-off key (typed only), the
// interrupt character, the rest buffered. Returns how many bytes were taken before it was full.
static size_t _put(const uint8_t *buf, size_t n, bool typed) {
    pthread_mutex_lock(&_rx_lock);
    uint32_t head = __atomic_load_n(&_rx_head, __ATOMIC_RELAXED);
    size_t i;
    for (i = 0; i < n; i++) {
        if (typed && _tty && buf[i] == CTRL_BACKSLASH) {
            desktop_power_off(0);
        }
        if (buf[i] == mp_interrupt_char) {
            mp_sched_keyboard_interrupt();
            continue;
        }
        uint32_t next = (head + 1) % RX_SIZE;
        if (next == __atomic_load_n(&_rx_tail, __ATOMIC_ACQUIRE)) {
            break;
        }
        _rx[head] = buf[i];
        head = next;
    }
    __atomic_store_n(&_rx_head, head, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&_rx_lock);
    desktop_wake_vm();
    return i;
}

// Typed input beyond a full buffer is dropped, as a UART's would be.
static void _received(const uint8_t *buf, size_t n) {
    _put(buf, n, true);
}

size_t desktop_serial_inject(const uint8_t *buf, size_t n) {
    // Waits while the VM reads, so long input (an exec's code) arrives whole and not mixed with
    // another client's; gives up after 10 s without progress.
    static pthread_mutex_t whole = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&whole);
    size_t done = 0;
    for (int idle_ms = 0; done < n && idle_ms < 10000;) {
        size_t took = _put(buf + done, n - done, false);
        done += took;
        if (took == 0) {
            struct timespec t = { 0, 1000000L };
            nanosleep(&t, NULL);
            idle_ms++;
        } else {
            idle_ms = 0;
        }
    }
    pthread_mutex_unlock(&whole);
    return done;
}

void desktop_serial_restore(void) {
    platform_console_restore();
}

void port_serial_init(void) {
    _tty = platform_console_open(_received);
}

void port_serial_write_substring(const char *text, uint32_t len) {
    control_api_console(text, len);
    platform_console_write(text, len);
}

void port_serial_early_init(void) {
}

bool port_serial_connected(void) {
    return true;
}

uint32_t port_serial_bytes_available(void) {
    uint32_t head = __atomic_load_n(&_rx_head, __ATOMIC_ACQUIRE);
    uint32_t count = (head + RX_SIZE - _rx_tail) % RX_SIZE;
    if (count > 0) {
        _empty_polls = 0;
    } else if (++_empty_polls >= IDLE_POLLS) {
        // The console input loop polls without pausing; on a host that would spin a core.
        _empty_polls = 0;
        desktop_wait_for_interrupt(10);
    }
    return count;
}

char port_serial_read(void) {
    uint32_t head = __atomic_load_n(&_rx_head, __ATOMIC_ACQUIRE);
    if (head == _rx_tail) {
        return -1;
    }
    char c = _rx[_rx_tail];
    __atomic_store_n(&_rx_tail, (_rx_tail + 1) % RX_SIZE, __ATOMIC_RELEASE);
    return c;
}
