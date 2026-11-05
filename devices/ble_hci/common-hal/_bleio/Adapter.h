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

typedef struct _bleio_adapter_obj_t {
    mp_obj_base_t base;
    bleio_scanresults_obj_t *scan_results;
    mp_obj_t name;
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

    // All the local attributes for this device. The index into the list
    // corresponds to the handle.
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
void bleio_adapter_advertising_was_stopped(bleio_adapter_obj_t *self);
mp_obj_t *bleio_adapter_get_attribute(bleio_adapter_obj_t *adapter, uint16_t handle);
uint16_t bleio_adapter_max_attribute_handle(bleio_adapter_obj_t *adapter);
void bleio_adapter_background(bleio_adapter_obj_t *adapter);
void bleio_adapter_gc_collect(bleio_adapter_obj_t *adapter);
void bleio_adapter_reset(bleio_adapter_obj_t *adapter);
// An advertising report from a scan (hci.c), into the scan's results while it runs.
void bleio_adapter_scan_report(bleio_adapter_obj_t *self, bool connectable, bool scan_response,
    int8_t rssi, const bt_addr_le_t *addr, const uint8_t *data, uint8_t len);
