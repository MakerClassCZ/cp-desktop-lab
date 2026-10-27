// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Command-line options, parsed in desktop_main.c.
typedef struct {
    const char *drive_path;     // CIRCUITPY: a directory or a FAT image, absolute
    const char *start_dir;      // the working directory at start; VfsPosix changes it
    size_t drive_size;          // bytes, used only when the image is created
    size_t ram_size;            // bytes of RAM for the port heap
    int debug_port;             // --debug: the debug adapter's TCP port, 0 = none
    const char *exe_dir;        // the program's own directory, where debugadapter.py lives
    const char *board_name;     // --board: "virtual" (default), "ft232h", "mcp2221" or "gpiochip"
    const char *trace_bus;      // --trace-bus: the file busio transfers are written to, or NULL
    const char *uart_path;      // --uart: the host serial port behind board.TX and board.RX, or NULL
    const char *gpiochip;       // --gpiochip: the GPIO chip of --board gpiochip
    int i2c_bus;                // --i2c-bus: its /dev/i2c-N
    const char *spidev;         // --spi: its spidev device
    const char *board_files[8]; // --board-file, read in order: pin names, NeoPixel layouts, parts
    int board_file_count;
    const char *neopixel[8];    // --neopixel NAME=LAYOUT, as given
    int neopixel_count;
    int api_port;               // --api: the control API's port, 0 = any free one, -1 = off
    const char *api_host;       // --api-host: the address it listens on, NULL = 127.0.0.1
    bool fast_time;             // --fast-time: sleeps return at once and move the program's clock on
    char **argv;                // reset re-executes the process with these
} desktop_options_t;

extern desktop_options_t desktop_options;

// The board chosen at run time (desktop_board.c): its board module table and its pin backend.
bool desktop_board_select(const char *name);
// sys.platform as a real board names it (a board file's [board] platform).
bool desktop_set_platform(const char *name);
// os.uname().machine as a real board names it (a board file's [board] machine).
bool desktop_set_machine(const char *name);
const char *desktop_board_names(void);
// --board-file and --neopixel on the chosen board (board_file.c); false: the cause is printed.
bool board_file_apply(void);
// --list-boards: every board and whether its adapter is plugged in, one per line, tab-separated.
void desktop_board_list(void);
// The serial port busio.UART opens: --uart, else on --board mcp2221 the chip's own; NULL = none.
const char *desktop_uart_path(void);

// The VM's part of the main thread's stack, set up in desktop_main.c.
extern uint8_t *desktop_stack_bottom;
extern uint8_t *desktop_stack_top;

// The process was restarted by reset_cpu() rather than started fresh.
extern bool desktop_was_reset;

// Whether the caller runs on the VM thread (the main thread), where the pins' USB device may be used.
bool desktop_on_vm_thread(void);

// Interrupts (interrupts.c): the timer thread, its lock, and waking the VM out of idle.
bool desktop_interrupts_init(void);
void desktop_irq_disable(void);
void desktop_irq_enable(void);
void desktop_wake_vm(void);

// Run the workflow's background poll after ms, from a timer interrupt.
void desktop_request_workflow_poll(uint32_t ms);

// Block until something wakes the VM (desktop_wake_vm) or timeout_ms passes (-1 = no timeout).
void desktop_wait_for_interrupt(int timeout_ms);

// Fast time: the sleep skipped so far, which the program's clock is ahead of the host's by;
// skipping, on the VM thread, runs the background tasks once.
uint64_t desktop_skipped_ns(void);
void desktop_skip_ns(uint64_t ns);
// The program's clock: the host's monotonic clock plus the skipped sleep, in ns.
uint64_t desktop_program_ns(void);
// The host's monotonic clock in ms, for what paces itself to a person watching.
uint64_t desktop_host_ms(void);
void desktop_host_sleep_ms(unsigned ms);
// The program's wall time in seconds: the host's plus the skipped sleep.
int64_t desktop_program_time(void);

// The console's buttons, in board pin order (GPIO0 = UP ... GPIO12 = MENU).
enum {
    DESKTOP_BUTTON_UP, DESKTOP_BUTTON_DOWN, DESKTOP_BUTTON_LEFT, DESKTOP_BUTTON_RIGHT,
    DESKTOP_BUTTON_A, DESKTOP_BUTTON_B, DESKTOP_BUTTON_X, DESKTOP_BUTTON_Y,
    DESKTOP_BUTTON_START, DESKTOP_BUTTON_SELECT, DESKTOP_BUTTON_L, DESKTOP_BUTTON_R, DESKTOP_BUTTON_MENU,
    DESKTOP_BUTTON_COUNT
};

// A button pulls its pin low while pressed, like a button to ground.
void desktop_button_set(int button, bool pressed);
// A button a key, controller or the panel holds down now.
bool desktop_button_held(int button);
void desktop_buttons_release_all(void);

// Watch the CIRCUITPY directory and auto-reload when the host changes a file in it; once.
void desktop_watch_start(const char *root);
// For each system's watcher (platform/*/watch.c): a host change seen, its thread started.
void desktop_watch_changed(void);
void desktop_watch_thread(void *(*main)(void *), void *arg);

void desktop_serial_restore(void);
__attribute__((noreturn)) void desktop_power_off(int status);
