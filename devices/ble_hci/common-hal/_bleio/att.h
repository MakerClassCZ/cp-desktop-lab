// This file is part of the CircuitPython project: https://circuitpython.org

// SPDX-FileCopyrightText: Copyright (c) 2020 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Arduino SA. All rights reserved.
//
// SPDX-License-Identifier: LGPL-2.1-or-later

//  This file is derived from the ArduinoBLE library. Its header is below.

/*
  This file is part of the ArduinoBLE library.
  Copyright (c) 2018 Arduino SA. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hci_include/addr.h"
#include "hci_include/att.h"
#include "hci_include/att_internal.h"

void bleio_att_reset(void);

// FIX void att_set_event_handler(BLEDeviceEvent event, BLEDeviceEventHandler eventHandler);
bool att_address_is_connected(bt_addr_le_t *addr);
bool att_connect_to_address(bt_addr_le_t *addr);
bool att_disconnect(uint16_t conn_handle);
bool att_disconnect_all(void);
bool att_discover_attributes(bt_addr_le_t *addr, const char *service_uuid_filter);
bool att_exchange_mtu(uint16_t conn_handle);
bool att_handle_is_connected(uint16_t handle);
bool att_indicate(uint16_t handle, const uint8_t *value, int length);
bool att_is_connected(void);
bool att_notify(uint16_t handle, const uint8_t *value, int length);
// The client: each request waits up to the ATT timeout for its answer, which fills
// response_buffer (cut at its capacity): its length, 0 when none came (or the link went).
// ATT_CLIENT_RSP_MAX holds any answer the host takes.
#define ATT_CLIENT_RSP_MAX 255
int att_find_info_req(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle, uint8_t response_buffer[], size_t response_capacity);
int att_find_type_req(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle, uint16_t type, const uint8_t *value, size_t value_len, uint8_t response_buffer[], size_t response_capacity);
int att_read_blob_req(uint16_t conn_handle, uint16_t handle, uint16_t offset, uint8_t response_buffer[], size_t response_capacity);
int att_read_group_req(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle, uint16_t uuid, uint8_t response_buffer[], size_t response_capacity);
int att_read_req(uint16_t conn_handle, uint16_t handle, uint8_t response_buffer[], size_t response_capacity);
int att_read_type_req(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle, uint16_t type, uint8_t response_buffer[], size_t response_capacity);
// A remote value read whole (Read, then Read Blob), up to len bytes: its length. Written in one
// PDU (a Write Request, or a Write Command without response); longer than MTU - 3 raises.
// Both raise on an Error Response, no answer, or the link gone.
size_t att_read_value(uint16_t conn_handle, uint16_t handle, uint8_t *buf, size_t len);
void att_write_value(uint16_t conn_handle, uint16_t handle, const uint8_t *data, size_t len, bool with_response);
// Raises the error of an ATT error code: BluetoothError, or SecurityError for authentication
// and encryption.
void att_check_error(uint8_t err);
// GATT characteristic properties as _bleio's.
uint8_t att_properties_to_bleio(uint8_t ble_spec_properties);
int att_write_req(uint16_t conn_handle, uint16_t handle, const uint8_t *data, uint8_t data_len, uint8_t response_buffer[], size_t response_capacity);
uint16_t att_conn_handle(bt_addr_le_t *addr);
uint16_t att_mtu(uint16_t handle);
void att_add_connection(uint16_t handle, uint8_t role, bt_addr_le_t *peer_addr, uint16_t interval, uint16_t latency, uint16_t supervision_timeout, uint8_t master_clock_accuracy);
void att_process_data(uint16_t conn_handle, uint8_t dlen, uint8_t data[]);
void att_remove_connection(uint16_t conn_handle, uint8_t reason);
void att_set_max_mtu(uint16_t max_mtu);
void att_set_timeout(unsigned long timeout);
void att_write_cmd(uint16_t conn_handle, uint16_t handle, const uint8_t *data, uint8_t data_len);
