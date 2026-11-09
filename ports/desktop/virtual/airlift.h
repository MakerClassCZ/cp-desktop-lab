// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// An AirLift: an ESP32 running nina-fw on the virtual SPI bus, as adafruit_esp32spi drives it.
// Its ready pin (BUSY) is low when it takes a command, high from chip select until it has
// answered; its network is the computer's (airlift_net.c).

// The board file's line: one per board. False with *why set.
bool airlift_add(int cs, int busy, int reset, int gpio0, const char **why);
// One of its pins.
bool airlift_uses_pin(int pin);
// Its chip select is low: the transfer is its.
bool airlift_selected(void);
void airlift_transfer(const uint8_t *out, uint8_t fill, uint8_t *in, size_t len);
// The level of its ready pin; false for other pins. No side effects: the panel reads it too.
bool airlift_pin_level(uint8_t pin, bool *level);
// A pin the program drives changed: chip select, RESET.
void airlift_pin_changed(uint8_t pin);
// From the VM loop: a command waiting on the network finished, a TLS handshake's next step.
void airlift_vm_task(void);
// With the VM: its sockets closed, as a reset of the ESP32.
void airlift_reset(void);
