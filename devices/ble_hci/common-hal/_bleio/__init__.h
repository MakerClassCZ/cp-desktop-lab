// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Dan Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>

#include "shared-bindings/_bleio/UUID.h"

#include "att.h"
#include "hci.h"

void bleio_hci_background(void);

// We assume variable length data.
// 20 bytes max (23 - 3).
#define GATT_MAX_DATA_LENGTH (BT_ATT_DEFAULT_LE_MTU - 3)

// FIX
#define BLE_GATT_HANDLE_INVALID 0x0000
#define BLE_CONN_HANDLE_INVALID 0xFFFF
#define BLE_GATTS_FIX_ATTR_LEN_MAX (510)  /**< Maximum length for fixed length Attribute Values. */
#define BLE_GATTS_VAR_ATTR_LEN_MAX (512)  /**< Maximum length for variable length Attribute Values. */

// Track if the user code modified the BLE state to know if we need to undo it on reload.
extern bool vm_used_ble;
// The program added services: with the BLE workflow on, its reload restarts the stack.
extern bool vm_added_services;

// UUID shared by all CCCD's.
extern bleio_uuid_obj_t cccd_uuid;
