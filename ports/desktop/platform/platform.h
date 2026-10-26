// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the supervisor and the process entry need from the system, implemented once per system
// in platform/posix/ and platform/windows/.

// The console (console.c): stdin raw when it is a terminal, a reader thread that hands what it
// reads to received(), stdout, and the terminal as it was. open returns whether stdin is a
// terminal.
bool platform_console_open(void (*received)(const uint8_t *data, size_t len));
void platform_console_write(const char *text, uint32_t len);
void platform_console_restore(void);

// The process (process.c).
// Sets an environment variable for this process and the ones it starts; NULL removes it.
void platform_set_env(const char *name, const char *value);
// A crash prints a backtrace on stderr and calls restart, which does not return.
void platform_catch_faults(void (*restart)(void));
// The same program again with the same arguments, as a reset; generation counts the processes
// since the fresh start. Returns only when it could not.
void platform_restart(int generation);
// Before anything else runs: the console's code page, the termination signals or events, the
// network library, the drive path in the system's form. False ends the program.
bool platform_start(char *drive_path);
// The system's limit on the main thread's stack, 0 for none.
size_t platform_stack_limit(void);
// The directory of the program's own file, or ".".
void platform_exe_dir(char *dir, size_t len);
bool platform_is_absolute(const char *path);
// The thread that runs the VM: marked once, from it.
void platform_mark_vm_thread(void);

// The drive image (drive.c): opened or created at new_size, mapped whole, trimmed to a multiple
// of block bytes; the program ends with a message when it cannot be.
uint8_t *platform_map_drive(const char *path, size_t new_size, size_t block, size_t *size);
void platform_flush_drive(uint8_t *drive, size_t size);
