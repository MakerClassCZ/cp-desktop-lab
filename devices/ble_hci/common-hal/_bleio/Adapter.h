// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Artur Pacholec
// SPDX-FileCopyrightText: Copyright (c) 2016 Glenn Ruben Bakke
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "py/objtuple.h"

#include "shared-bindings/_bleio/Address.h"
#include "shared-bindings/_bleio/Characteristic.h"
#include "shared-bindings/_bleio/Connection.h"
#include "shared-bindings/_bleio/ScanResults.h"
#include "shared-bindings/busio/UART.h"
#include "shared-bindings/digitalio/DigitalInOut.h"
#include "common-hal/_bleio/hci_transport.h"

#ifndef BLEIO_TOTAL_CONNECTION_COUNT
#define BLEIO_TOTAL_CONNECTION_COUNT 5
#endif

extern bleio_connection_internal_t bleio_connections[BLEIO_TOTAL_CONNECTION_COUNT];

// The attribute table's first part: _bleio's own GAP and GATT services and the BLE workflow's,
// added from an enable until the program adds a service. It is off the VM heap (static objects,
// their values and lists in a static arena), so it outlives a VM and the handles in it stay the
// same from one run to the next; the program's attributes follow it, on the VM's heap.
#ifndef BLEIO_HCI_STATIC_ATTRIBUTES
#define BLEIO_HCI_STATIC_ATTRIBUTES 40
#endif

typedef struct _bleio_adapter_obj_t {
    mp_obj_base_t base;
    bleio_scanresults_obj_t *scan_results;
    mp_obj_tuple_t *connection_objs;
    // The link to the controller, and what its callbacks are given.
    const bleio_hci_transport_t *hci_transport;
    void *hci_transport_ctx;
    // The UART transport's link (bleio_hci_uart_transport): unused by any other.
    busio_uart_obj_t *hci_uart;
    digitalio_digitalinout_obj_t *rts_digitalinout;
    digitalio_digitalinout_obj_t *cts_digitalinout;
    bool allocated;  // True when in use.
    // A port's own adapter (its controller built in, not one the program wires up): a reset
    // disables it but keeps it allocated and bound to _bleio.adapter for the next run.
    bool persistent;
    bool now_advertising;
    bool circuitpython_advertising;
    bool enabled;

    // HCI adapter version info.
    uint8_t hci_version;
    uint8_t lmp_version;
    uint16_t hci_revision;
    uint16_t manufacturer;
    uint16_t lmp_subversion;

    // Used to monitor advertising timeout for legacy advertising.
    uint64_t advertising_start_ticks;
    uint64_t advertising_timeout_msecs;  // If zero, do not check.

    // The scan's start and timeout, which bleio_adapter_background() ends it at; 0: no timeout.
    uint64_t scan_start_ticks;
    uint64_t scan_timeout_msecs;
    // A connection the program asked for: -1 while waiting, then LE Connection Complete's status.
    volatile int16_t connect_status;

    // Generic services characteristics.
    bleio_characteristic_obj_t *device_name_characteristic;
    bleio_characteristic_obj_t *appearance_characteristic;
    bleio_characteristic_obj_t *service_changed_characteristic;

    uint16_t max_acl_buffer_len;
    uint16_t max_acl_num_buffers;
    uint8_t features[8];        // Supported BLE features.

    // The local attributes; an attribute's handle is its index. The first static_attribute_count
    // are in static_attributes, the rest in attributes (the program's, NULL when it has none).
    mp_obj_t static_attributes[BLEIO_HCI_STATIC_ATTRIBUTES];
    uint16_t static_attribute_count;
    // Attributes still go to the static part: from an enable until the program adds a service.
    bool static_attributes_open;
    mp_obj_list_t *attributes;
    // Handle for last added service. Characteristics can only be added immediately after
    // the service they belong to. This vets that.
    uint16_t last_added_service_handle;
    uint16_t last_added_characteristic_handle;
    // Cached local address returned by common_hal_bleio_adapter_get_address().
    // Stored inline so it never needs to allocate, even when read before the
    // heap is available (e.g. from bleio_adapter_reset_name).
    bleio_address_obj_t address;
    // BT_ADDR_LE_PUBLIC, or BT_ADDR_LE_RANDOM with random_address for a controller that has no
    // public address.
    uint8_t own_addr_type;
    bt_addr_t random_address;
    // The controller's identity address (its public one, or the static one when it has none), as
    // the last enable found it; the bonds made through it are filed under it. Type and value 0
    // until an enable has read it.
    bt_addr_le_t identity;
    bool identity_known;
    // The address the last advertising went out from: ours on a link a central makes to it.
    bt_addr_le_t advertising_address;
    // Read Local Supported Commands' bitmap (all 0 when the controller did not answer it).
    uint8_t supported_commands[64];
} bleio_adapter_obj_t;

uint16_t bleio_adapter_add_attribute(bleio_adapter_obj_t *adapter, mp_obj_t *attribute);
// The program adds a service: the static part of the table ends.
void bleio_adapter_close_static_attributes(bleio_adapter_obj_t *adapter);
// The table cut back to its static part: what the program added goes with its heap.
void bleio_adapter_cut_attributes(bleio_adapter_obj_t *adapter);
// Memory for what an attribute object owner holds (its value, its lists, its CCCD): from the VM
// heap for an owner on the heap (the program's), else from the static arena, which the next
// enable starts over.
void *bleio_attribute_alloc(const void *owner, size_t size);
// An attribute's value: a bytearray of capacity bytes (at least 1), set to initial (may be NULL);
// written in place from then on, so an ATT write allocates nothing.
mp_obj_t bleio_attribute_value_new(const void *owner, size_t capacity, const mp_buffer_info_t *initial);
void bleio_attribute_value_set(mp_obj_t value, const uint8_t *data, size_t len);
// A list for owner: on the heap an empty one that grows; in the arena one of alloc items that
// never does.
mp_obj_list_t *bleio_attribute_list_new(const void *owner, size_t alloc);
void bleio_adapter_advertising_was_stopped(bleio_adapter_obj_t *self);
mp_obj_t *bleio_adapter_get_attribute(bleio_adapter_obj_t *adapter, uint16_t handle);
uint16_t bleio_adapter_max_attribute_handle(bleio_adapter_obj_t *adapter);
void bleio_adapter_background(bleio_adapter_obj_t *adapter);
void bleio_adapter_gc_collect(bleio_adapter_obj_t *adapter);
void bleio_adapter_reset(bleio_adapter_obj_t *adapter);
// An advertising report from a scan (hci.c), into the scan's results while it runs.
void bleio_adapter_scan_report(bleio_adapter_obj_t *self, bool connectable, bool scan_response,
    int8_t rssi, const bt_addr_le_t *addr, const uint8_t *data, uint8_t len);
