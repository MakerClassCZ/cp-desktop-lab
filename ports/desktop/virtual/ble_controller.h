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
// Thread-safe: the VM thread speaks HCI, the control API's threads play the air around it.
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
// The oldest frame the program sent the phone on channel cid (ATT and SMP have a queue each;
// every channel is in the events), its length; 0 when there is none.
size_t ble_controller_l2cap_take(uint16_t cid, uint8_t *pdu, size_t max);
size_t ble_controller_att_take(uint8_t *pdu, size_t max);
// The peer of address (NULL: the phone) starts encryption of its link with ltk, ediv and rand:
// the link's number, or -1 when there is no such link. Its result comes with the host's answer:
// ble_controller_encrypt_result(link) is -1 until then, then 0 (encrypted), 0x06 (the host has
// no such key), 0x3d (another key: the link is gone) or the reason the link ended meanwhile.
int ble_controller_encrypt(const uint8_t *address, const uint8_t ltk[16], uint16_t ediv, const uint8_t rand[8]);
int ble_controller_encrypt_result(int link);

// The air: peers around the program. Addresses least significant byte first.
typedef struct {
    uint8_t address[6];
    bool random;
    bool advertising;           // false: there, its link kept, but silent
    bool gone;                  // out of the air: its link ends (0x13)
    bool connectable;
    uint8_t data[31];
    uint8_t data_len;
    uint8_t scan_response[31];
    uint8_t scan_response_len;
    int8_t rssi;
} ble_peer_t;
// A peer posted: new, or advertising something new (reported again to a scan, and it completes a
// connection the program waits for). -1 when the air is full.
int ble_controller_peer(const ble_peer_t *peer);
// A peer connects to what the program advertises, as a central: 0, or -1 when the program
// advertises nothing connectable or the links are all in use.
int ble_controller_peer_connect(const uint8_t address[6], bool random);
// The link to a peer ends, as the peer ends it: 0, or -1 when there is none.
int ble_controller_peer_disconnect(const uint8_t address[6], uint8_t reason);
// An L2CAP frame's payload from a peer to the program on channel cid: 0, or -1 with no link.
int ble_controller_l2cap(const uint8_t address[6], uint16_t cid, const uint8_t *data, size_t len);
bool ble_controller_scanning(void);

// What happens in the air, in one order: the program's advertising, its scanning, its links
// and every L2CAP frame it sends on one.
enum {
    BLE_EVENT_ADV,
    BLE_EVENT_SCAN,
    BLE_EVENT_CONNECTED,
    BLE_EVENT_DISCONNECTED,
    BLE_EVENT_L2CAP,
};
#define BLE_EVENT_DATA_MAX 512
typedef struct {
    uint64_t seq;
    uint8_t kind;
    bool on;                    // adv, scan
    bool connectable;           // adv
    bool active;                // scan
    bool encrypted;             // connected
    uint8_t role;               // the program's on the link: 0 central, 1 peripheral
    uint16_t handle;
    uint8_t reason;             // disconnected
    uint16_t cid;               // l2cap
    uint8_t address[6];         // adv: the program's; else the peer's
    bool address_random;
    uint8_t local[6];           // connected: the program's own on the link
    bool local_random;
    uint8_t scan_response_len;  // adv: its scan response at data + 31
    uint16_t len;
    uint8_t data[BLE_EVENT_DATA_MAX];
} ble_event_t;
// The events from number since, up to max, waiting up to wait_ms for one: their count; *next
// is the number to ask from next, *lost true when some before were gone from the ring.
size_t ble_controller_events(uint64_t since, int wait_ms, ble_event_t *out, size_t max, uint64_t *next, bool *lost);
