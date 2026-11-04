// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimír Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

// The CYW43439's Bluetooth core as an HCI controller: H4 packets in and out over the shared bus
// that Wi-Fi uses. Plain C over the cyw43 driver, no CircuitPython types, so firmware other than
// CircuitPython's can use it too. Each call takes the cyw43 driver's lock itself.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Loads the Bluetooth firmware the first time; 0 when the core is up.
int cyw43_hci_init(void);
// Drops what was received and stops queueing more. The core stays up: the driver cannot stop it.
void cyw43_hci_deinit(void);
// One H4 packet, its type byte first. false: too long, or the bus refused it.
bool cyw43_hci_send(const uint8_t *h4, size_t len);
// One H4 packet into h4, type byte first, or 0 when none is waiting. A packet longer than max is
// dropped (and counted).
size_t cyw43_hci_receive(uint8_t *h4, size_t max);

// Packets the receive queue had no room for, or receive no room to give back.
extern uint32_t cyw43_hci_dropped;
