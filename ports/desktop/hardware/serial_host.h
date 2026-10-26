// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A serial port of the host: COMn on Windows, a tty on POSIX. busio.UART runs on it.
typedef struct {
    intptr_t handle;            // a HANDLE or a file descriptor; -1 when closed
} serial_host_t;

// err gets a message naming the port when it cannot be opened.
bool serial_host_open(serial_host_t *port, const char *path, char *err, size_t err_len);
void serial_host_close(serial_host_t *port);
// parity: 'N', 'E' or 'O'. False when the host cannot use these settings (a baud rate it lacks).
bool serial_host_configure(serial_host_t *port, uint32_t baud, uint8_t bits, char parity, uint8_t stop);
// What has arrived, at most len bytes, waiting up to timeout_ms for the first. -1 = an error.
int serial_host_read(serial_host_t *port, uint8_t *data, size_t len, int timeout_ms);
// The bytes written, or -1.
int serial_host_write(serial_host_t *port, const uint8_t *data, size_t len, int timeout_ms);
int serial_host_available(serial_host_t *port);
void serial_host_flush_input(serial_host_t *port);

// The serial port of a USB device (its CDC interface), by VID and PID: "COM7" or "/dev/ttyACM0".
bool serial_host_find_usb(uint16_t vid, uint16_t pid, char *path, size_t len);
