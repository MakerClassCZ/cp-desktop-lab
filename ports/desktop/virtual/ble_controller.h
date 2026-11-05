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
// The phone connects from address (6 bytes, least significant first) of type (0 public, 1
// random), or NULL for its own: 0, or -1 when the program advertises nothing connectable or a
// phone is connected already.
int ble_controller_connect(const uint8_t *address, uint8_t type);
void ble_controller_disconnect(uint8_t reason);
// An L2CAP frame from the phone to the program on channel cid; false when not connected.
bool ble_controller_l2cap_send(uint16_t cid, const uint8_t *pdu, size_t len);
bool ble_controller_att_send(const uint8_t *pdu, size_t len);
// The oldest frame the program sent the phone on channel cid (ATT and SMP have a queue each, the
// other channels one together: got_cid says which), its length; 0 when there is none.
size_t ble_controller_l2cap_take(uint16_t cid, uint16_t *got_cid, uint8_t *pdu, size_t max);
size_t ble_controller_att_take(uint8_t *pdu, size_t max);
// The phone starts encryption with ltk, ediv and rand: 0, or -1 when it is not connected. Its
// result comes with the host's answer: ble_controller_encrypt_result() is -1 until then, then 0
// (encrypted), 0x06 (the host has no such key) or 0x3d (another key: the link is gone).
int ble_controller_encrypt(const uint8_t ltk[16], uint16_t ediv, const uint8_t rand[8]);
int ble_controller_encrypt_result(void);
