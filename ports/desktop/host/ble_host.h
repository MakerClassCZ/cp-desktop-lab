// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The HCI link of _bleio (devices/ble_hci), H4 bytes both ways, behind the adapter's transport
// (hci_transport.h): read and write never wait, they move what there is, 0 when nothing, -1 when
// the link is gone; a write is one whole packet.
int ble_host_available(void);
int ble_host_read(uint8_t *data, size_t len);
int ble_host_write(const uint8_t *data, size_t len);

// A board file's [board] ble: "on" or "off"; false (and the cause printed) for anything else.
bool ble_host_board_setting(const char *value);
// The serial port --ble names, or NULL for the virtual controller or none.
const char *ble_host_port(void);
// The controller --ble chose, opened before anything runs: the virtual one (no --ble),
// a serial port, or none (--ble off, or the board file's off). False: the cause is printed.
bool ble_host_init(void);
// _bleio.adapter bound to it, disabled, kept across reloads; nothing without a controller.
void ble_host_attach_adapter(void);
// "virtual", "serial" (--ble PORT) or "off".
const char *ble_host_controller(void);

// A connection as the HCI shows it.
#define BLE_HOST_LINKS 4
typedef struct {
    uint16_t handle;
    bool central;               // the program's role on it: central, else peripheral
    bool encrypted;
    uint8_t key_size;           // of its encryption
    uint8_t peer[6];            // least significant byte first
} ble_host_link_t;

// What the HCI shows, the same for every controller (host/ble_watch.c).
typedef struct {
    bool advertising;
    bool connectable;
    bool connected;
    bool scanning;
    bool public;              // address is the controller's public one, else the random one
    uint8_t address[6];         // the address advertising uses, least significant byte first
    uint8_t peer[6];            // the one connected
    char name[32];              // the advertised local name
    char central_name[32];      // the peer of the last connection the program made, by name
    uint8_t data[31];
    uint8_t data_len;
    uint8_t scan_response[31];
    uint8_t scan_response_len;
    ble_host_link_t links[BLE_HOST_LINKS];  // the open connections, in the order they came
    uint8_t link_count;
    uint8_t bonds;              // in the bond store, of the controller's identity
} ble_host_state_t;

// Every complete H4 packet, to_controller or from it: the state, the trace, the log.
void ble_host_packet(bool to_controller, const uint8_t *h4, size_t len);
void ble_host_state(ble_host_state_t *state);
// --ble-log PATH: every packet as btsnoop; false (the cause printed) when it does not open.
bool ble_host_open_log(const char *path);
// The bond store's blob as it was read or written (VM thread): its bonds of the controller's
// identity counted for the state, and in the trace what changed since the last one.
void ble_host_bonds_stored(const uint8_t *blob, size_t len, bool written);
