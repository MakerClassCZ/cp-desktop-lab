// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api_json.h"

// --api: a local HTTP interface to the running program for editors, tests and agents: the
// console, the REPL, the board panel, the displays and the trace (README, "Controlling the
// program"). The server runs on threads of its own; what needs the VM is queued for it.

// Opens the port and starts the server; false: the reason is printed.
bool control_api_start(void);
// Closes the listening socket, so a process started by a reset can take the port.
void control_api_stop(void);
// Before the process ends: up to 300 ms for a reader of the console to have its last bytes.
void control_api_linger(void);
// The requests that need the VM (the panel, the state, the displays), run on the VM thread from
// the background task; returns at once when there are none.
void control_api_vm_task(void);
// /api/info's answer, for lab.info too: the version, the board, its profile and limits, fast
// time, the process.
void control_api_info_json(api_buf_t *b);
// What the console prints, kept for the API's readers; nothing when the API is off.
void control_api_console(const char *text, size_t len);

// The console's input, as if typed (supervisor/serial.c); waits while the input buffer is full.
// Returns how much went in (less when the VM stopped reading for 10 s).
size_t desktop_serial_inject(const uint8_t *buf, size_t n);
