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
// A board file's [board] ble_workflow: on, discover or off; false (the cause printed) for others.
bool ble_host_board_workflow(const char *value);
// The BLE workflow's mode: "on", "discover" or "off" (--ble-workflow, else the board file's).
const char *ble_host_workflow_mode(void);
// From port_init(): _bleio.adapter bound to the controller, disabled, kept across reloads (nothing
// without one); discover's saved word (once per launch); the stand-in bond (CIRCUITPY_BLE_BONDED).
void ble_host_port_init(void);
// From board_init(): the workflow switched off unless asked for.
void ble_host_board_init(void);
// On the VM thread, at every background pass: discovery mode the API asked for (a reset).
void ble_host_vm_task(void);
// From the API: discovery mode, as reset plus the boot button: false when the workflow is off.
bool ble_host_request_discovery(void);
// Before a reset re-executes the process: a stand-in bond erased stays erased.
void ble_host_before_reset(void);
// The workflow's state: "off", "discovery", "reconnecting", "connected" or "on".
const char *ble_host_workflow_state(void);
// Whether the bond store holds a bond with a central (the workflow's "bonded").
bool ble_host_bonded(void);
// "virtual", "serial" (--ble PORT) or "off".
const char *ble_host_controller(void);

// A connection as the HCI shows it.
#define BLE_HOST_LINKS 4
typedef struct {
    uint16_t handle;
    bool central;               // the program's role on it: central, else peripheral
    bool encrypted;
    uint8_t peer[6];            // least significant byte first
} ble_host_link_t;

// What the HCI shows, the same for every controller (host/ble_watch.c).
typedef struct {
    bool advertising;
    bool connectable;
    bool connected;
    bool public;                // address is the controller's public one, else the random one
    uint8_t address[6];         // the address advertising uses, least significant byte first
    uint8_t peer[6];            // the one connected
    char name[32];              // the advertised local name
    uint8_t data[31];
    uint8_t data_len;
    uint8_t scan_response[31];
    uint8_t scan_response_len;
    ble_host_link_t links[BLE_HOST_LINKS];  // the open connections, in the order they came
    uint8_t link_count;
} ble_host_state_t;

// Every complete H4 packet, to_controller or from it: the state, the trace, the log.
void ble_host_packet(bool to_controller, const uint8_t *h4, size_t len);
void ble_host_state(ble_host_state_t *state);
// --ble-log PATH: every packet as btsnoop; false (the cause printed) when it does not open.
bool ble_host_open_log(const char *path);
