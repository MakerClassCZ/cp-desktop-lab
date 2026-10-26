// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "desktop_pins.h"

// An FT232H's MPSSE engine over usb_host: the FTDI requests, the command stream, and the GPIO
// shadow of both banks. Bank 0 = ADBUS (pins 0-7), bank 1 = ACBUS (pins 8-15).

bool mpsse_open(void);          // find the chip, enter MPSSE mode, sync; prints the cause on failure
void mpsse_close(void);         // back to the chip's default mode, release the device; idempotent
bool mpsse_lost(void);          // a USB error happened; every later call fails until a restart

// The command stream. send: one bulk OUT. recv: exactly len payload bytes, the status bytes the
// chip puts at the start of every packet stripped; false = lost or timed out.
bool mpsse_send(const uint8_t *cmd, size_t len);
bool mpsse_recv(uint8_t *buf, size_t len, unsigned timeout_ms);

// The clock divisor: 60 MHz / (2 (hz_div + 1)), or / 3 with three-phase clocking (I2C).
void mpsse_set_clock(uint32_t hz, bool three_phase);
uint32_t mpsse_clock_hz(void);

// GPIO through the shadow: direction, level and the drive-only-zero mask of each bank.
bool mpsse_gpio_set(uint8_t pin, bool output, bool value, bool drive_zero);
bool mpsse_gpio_get(uint8_t pin, bool *level);
void mpsse_gpio_reset(uint16_t keep_mask);
uint8_t mpsse_bank_value(uint8_t bank);     // the shadow, for a bus that writes a bank itself
uint8_t mpsse_bank_dir(uint8_t bank);

extern const desktop_pin_backend_t desktop_ft232h_backend;
extern const desktop_i2c_ops_t mpsse_i2c_ops;     // mpsse_i2c.c
extern const desktop_spi_ops_t mpsse_spi_ops;     // mpsse_spi.c
