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

// The phone's side, for the control API.
typedef struct {
    bool advertising;
    bool connectable;
    bool connected;
    bool public;                // address is the public one, else the random one in use
    uint8_t address[6];         // least significant byte first, as HCI has it
    uint8_t data[31];
    uint8_t data_len;
    uint8_t scan_response[31];
    uint8_t scan_response_len;
} ble_controller_state_t;

void ble_controller_state(ble_controller_state_t *state);
// 0, or -1 when the program advertises nothing connectable or a phone is connected already.
int ble_controller_connect(void);
void ble_controller_disconnect(uint8_t reason);
// An ATT PDU from the phone to the program; false when not connected.
bool ble_controller_att_send(const uint8_t *pdu, size_t len);
// The oldest ATT PDU the program sent the phone, its length; 0 when there is none.
size_t ble_controller_att_take(uint8_t *pdu, size_t max);
