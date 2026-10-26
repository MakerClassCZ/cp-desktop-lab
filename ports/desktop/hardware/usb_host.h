// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A USB device through libusb-1.0, loaded at run time so the program runs without it. One device
// at a time, interface 0. Every call runs on the VM thread.

#define USB_HOST_ERROR_TIMEOUT (-7)

// Load libusb; err gets "libusb-1.0 not found: <detail>" on failure.
bool usb_host_load(char *err, size_t err_len);

// Open the first device with this VID and PID and claim interface 0. err gets the cause.
bool usb_host_open(uint16_t vid, uint16_t pid, char *err, size_t err_len);
void usb_host_close(void);

// Whether such a device is plugged in, without opening it (false when libusb is missing).
bool usb_host_present(uint16_t vid, uint16_t pid);

// A vendor request to the device (bmRequestType 0x40), no data stage; < 0 = libusb error.
int usb_host_control(uint8_t request, uint16_t value, uint16_t index);

// Bulk transfers: bytes moved, or < 0 = libusb error (USB_HOST_ERROR_TIMEOUT with nothing moved).
int usb_host_bulk_out(uint8_t endpoint, const uint8_t *data, size_t len, unsigned timeout_ms);
int usb_host_bulk_in(uint8_t endpoint, uint8_t *buf, size_t len, unsigned timeout_ms);

// wMaxPacketSize of an endpoint of the open device, 0 when unknown.
int usb_host_max_packet(uint8_t endpoint);

const char *usb_host_strerror(int code);
