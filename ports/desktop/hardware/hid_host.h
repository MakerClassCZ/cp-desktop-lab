// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A USB HID device with 64-byte reports, through the system's HID support: hid.dll on Windows,
// /dev/hidraw on Linux, IOKit on macOS. One device at a time; every call runs on the VM thread.

#define HID_REPORT_SIZE 64

// Open the first device with this VID and PID, exclusively. err gets the cause.
bool hid_host_open(uint16_t vid, uint16_t pid, const char *what, char *err, size_t err_len);
void hid_host_close(void);

// Whether such a device is plugged in, without opening it.
bool hid_host_present(uint16_t vid, uint16_t pid);

// One report out and the device's answer in; false on an error or when nothing arrives in time.
bool hid_host_exchange(const uint8_t cmd[HID_REPORT_SIZE], uint8_t reply[HID_REPORT_SIZE], unsigned timeout_ms);

// One report out, no answer expected (a chip reset, after which the device re-enumerates).
bool hid_host_send(const uint8_t cmd[HID_REPORT_SIZE]);

// The last failure, for the message that marks the device lost.
const char *hid_host_last_error(void);
