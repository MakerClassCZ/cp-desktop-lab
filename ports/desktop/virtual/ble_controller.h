// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The virtual Bluetooth controller: H4 from the host (devices/ble_hci) in, H4 to it out.
// Thread-safe: the VM thread speaks HCI, the control API's threads play the phone.
void ble_controller_reset(void);
int ble_controller_available(void);
int ble_controller_read(uint8_t *data, size_t len);
int ble_controller_write(const uint8_t *data, size_t len);
