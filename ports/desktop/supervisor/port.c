// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The supervisor on a POSIX host: the process, its memory and its clock. Interrupts live in
// interrupts.c.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#define malloc_usable_size malloc_size
#else
#include <malloc.h>
#endif
#ifdef _WIN32
#define malloc_usable_size _msize
#endif

#include "supervisor/board.h"
#include "supervisor/filesystem.h"
#include "supervisor/port.h"
#include "supervisor/shared/safe_mode.h"
#include "supervisor/shared/tick.h"
#include "shared-bindings/microcontroller/__init__.h"
#include "shared-bindings/rtc/__init__.h"
#if CIRCUITPY_AUDIOIO
#include "common-hal/audioio/AudioOut.h"
#endif
#include "common-hal/microcontroller/Pin.h"

#include "control_api.h"
#include "desktop_pins.h"
#include "desktop_port.h"
#include "midi_in.h"
#include "parts.h"
#include "platform.h"

#define SAVED_WORD_ENV "CIRCUITPY_DESKTOP_SAVED_WORD"
#define RESTARTS_ENV "CIRCUITPY_DESKTOP_RESTARTS"
#define FAULTED_ENV "CIRCUITPY_DESKTOP_FAULTED"

bool desktop_was_reset;
static int _restarts;                   // process generations since the fresh start
static bool _restarted_after_fault;     // this process is the restart a hard fault asked for

// A fault right after a restart for a fault would restart forever; the second one ends the process.
static void _fault_restart(void) {
    if (_restarted_after_fault) {
        static const char msg[] = "circuitpython: hard fault again after the restart, giving up\r\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        desktop_power_off(3);
    }
    platform_set_env(FAULTED_ENV, "1");
    reset_into_safe_mode(SAFE_MODE_HARD_FAULT);
}

static size_t _ram_used;
static uint32_t _saved_word;
static uint64_t _start_ns;

safe_mode_t port_init(void) {
    _start_ns = desktop_program_ns();

    // reset_cpu() re-executes the process; the safe mode word rides along in the environment.
    const char *word = getenv(SAVED_WORD_ENV);
    if (word != NULL) {
        _saved_word = strtoul(word, NULL, 0);
        desktop_was_reset = true;
        platform_set_env(SAVED_WORD_ENV, NULL);
    }
    const char *restarts = getenv(RESTARTS_ENV);
    if (restarts != NULL) {
        _restarts = atoi(restarts);
        platform_set_env(RESTARTS_ENV, NULL);
    }
    if (getenv(FAULTED_ENV) != NULL) {
        _restarted_after_fault = true;
        platform_set_env(FAULTED_ENV, NULL);
    }

    // A crash is the host's hard fault: a backtrace on stderr, then a restart into safe mode.
    platform_catch_faults(_fault_restart);

    return desktop_interrupts_init() ? SAFE_MODE_NONE : SAFE_MODE_PROGRAMMATIC;
}

void reset_port(void) {
    #if CIRCUITPY_RTC
    rtc_reset();
    #endif
    #if CIRCUITPY_AUDIOIO
    audioout_reset();
    #endif
    parts_reset();
    midi_in_reset();
    reset_all_pins();
    #if CIRCUITPY_FILESYSTEM_POSIX
    // Once CIRCUITPY is up, a directory drive starts reloading on host edits.
    if (filesystem_present() && supervisor_filesystem_posix_root() != NULL) {
        desktop_watch_start(desktop_options.drive_path);
    }
    #endif
}

void reset_cpu(void) {
    // The next process listens on the API's port, with its token.
    control_api_stop();
    // The next process needs the pins' USB device; not from a fault handler, where libusb's locks
    // may be held.
    if (getenv(FAULTED_ENV) == NULL) {
        desktop_pin_backend->close();
    }
    desktop_serial_restore();
    char word[16];
    snprintf(word, sizeof(word), "%u", (unsigned)_saved_word);
    platform_set_env(SAVED_WORD_ENV, word);
    char count[16];
    snprintf(count, sizeof(count), "%d", _restarts + 1);
    platform_set_env(RESTARTS_ENV, count);
    // The relative paths of the command line are relative to where the process started.
    if (chdir(desktop_options.start_dir) != 0) {
        perror("circuitpython: reset");
    }
    platform_restart(_restarts + 1);
    _exit(1);
}

// There is no bootloader to go to: entering it ends the process.
void reset_to_bootloader(void) {
    desktop_power_off(0);
}

uint32_t *port_stack_get_limit(void) {
    return (uint32_t *)desktop_stack_bottom;
}

uint32_t *port_stack_get_top(void) {
    return (uint32_t *)desktop_stack_top;
}

// The port heap is the system allocator, limited to --ram bytes so a program sees the memory a
// board of that size would have. The VM heap is carved from it like on a board.
uint32_t *port_heap_get_bottom(void) {
    return NULL;
}

uint32_t *port_heap_get_top(void) {
    return NULL;
}

void port_heap_init(void) {
}

void *port_malloc(size_t size, bool dma_capable) {
    if (_ram_used + size > desktop_options.ram_size) {
        return NULL;
    }
    void *block = malloc(size);
    if (block != NULL) {
        _ram_used += malloc_usable_size(block);
    }
    return block;
}

void port_free(void *ptr) {
    if (ptr != NULL) {
        _ram_used -= malloc_usable_size(ptr);
        free(ptr);
    }
}

void *port_realloc(void *ptr, size_t size, bool dma_capable) {
    size_t old = ptr != NULL ? malloc_usable_size(ptr) : 0;
    if (size > old && _ram_used + size - old > desktop_options.ram_size) {
        return NULL;
    }
    void *block = realloc(ptr, size);
    if (block != NULL) {
        _ram_used = _ram_used - old + malloc_usable_size(block);
    }
    return block;
}

size_t port_heap_get_largest_free_size(void) {
    return _ram_used < desktop_options.ram_size ? desktop_options.ram_size - _ram_used : 0;
}

void port_set_saved_word(uint32_t value) {
    _saved_word = value;
}

uint32_t port_get_saved_word(void) {
    return _saved_word;
}

uint64_t port_get_raw_ticks(uint8_t *subticks) {
    uint64_t ns = desktop_program_ns() - _start_ns;
    uint64_t sub = ns / 1000000000 * 32768 + ns % 1000000000 * 32768 / 1000000000;
    if (subticks != NULL) {
        *subticks = sub & 31;
    }
    return sub >> 5;
}

void port_gc_collect(void) {
}

bool port_boot_button_pressed(void) {
    return false;
}

void desktop_power_off(int status) {
    // Release the pins on a clean end (the window closed, code.py over). Not from the signal
    // thread or the console handler: the VM thread may be inside a transfer.
    if (desktop_on_vm_thread()) {
        desktop_pin_backend->close();
    }
    desktop_serial_restore();
    exit(status);
}
