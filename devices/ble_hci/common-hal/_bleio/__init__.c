// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Artur Pacholec
// SPDX-FileCopyrightText: Copyright (c) 2016 Glenn Ruben Bakke
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/runtime.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Adapter.h"
#include "shared-bindings/_bleio/Characteristic.h"
#include "shared-bindings/_bleio/Connection.h"
#include "shared-bindings/_bleio/Descriptor.h"
#include "shared-bindings/_bleio/PacketBuffer.h"
#include "shared-bindings/_bleio/Service.h"
#include "shared-bindings/_bleio/UUID.h"
#include "supervisor/shared/bluetooth/bluetooth.h"

// UUID shared by all cccd's.
bleio_uuid_obj_t cccd_uuid;

bool vm_used_ble;
bool vm_added_services;

// void check_sec_status(uint8_t sec_status) {
//     if (sec_status == BLE_GAP_SEC_STATUS_SUCCESS) {
//         return;
//     }

//     switch (sec_status) {
//         case BLE_GAP_SEC_STATUS_UNSPECIFIED:
//             mp_raise_bleio_SecurityError(MP_ERROR_TEXT("Unspecified issue. The pairing request on the other device may have been declined or ignored."));
//             return;
//         default:
//             mp_raise_bleio_SecurityError(MP_ERROR_TEXT("Unknown security error: 0x%04x"), sec_status);
//     }
// }

void common_hal_bleio_init(void) {
    // Called on every import of _bleio. This is the only initialization HCI gets:
    // unlike other ports, nothing runs at board startup, because HCI is unusable
    // until user code supplies an adapter (UART, etc.) anyway.
    // Create a UUID object for all CCCD's.
    cccd_uuid.base.type = &bleio_uuid_type;
    common_hal_bleio_uuid_construct(&cccd_uuid, BLE_UUID_CCCD, NULL);
    // An adapter that is on keeps its state: every module that uses _bleio imports it again,
    // and a reset here dropped the connections (and a response being waited for) from the host
    // while the controller kept them; a port's own adapter keeps the BLE workflow's connection
    // too. bleio_reset() resets at the end of each run.
    if (!common_hal_bleio_adapter_obj.enabled) {
        bleio_hci_reset();
    }
}

// The BLE workflow is on and has the adapter: its services, its connection and their state go on
// from one VM to the next, as on nordic.
static bool workflow_running(void) {
    #if CIRCUITPY_BLE_FILE_SERVICE || CIRCUITPY_BLE_SERIAL_SERVICE
    return supervisor_bluetooth_workflow_is_enabled() && common_hal_bleio_adapter_obj.enabled;
    #else
    return false;
    #endif
}

// The VM ends, its heap still there. With the BLE workflow running, nordic's shape: the program's
// advertising and links end, and what it added to the attribute table goes; the workflow's
// connection, its MTU and CCCDs stay, and no HCI Reset is sent. Otherwise a full reset.
void bleio_user_reset(void) {
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    // What the program wrote last to its packet buffers goes out before they go; a send that
    // fails raises, and between two VMs no one would catch it.
    if (adapter->enabled) {
        nlr_buf_t flush;
        if (nlr_push(&flush) == 0) {
            bleio_packet_buffer_background();
            nlr_pop();
        }
    }
    if (!workflow_running()) {
        bleio_reset();
        return;
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        if (adapter->now_advertising && !adapter->circuitpython_advertising) {
            common_hal_bleio_adapter_stop_advertising(adapter);
        }
        for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
            bleio_connection_internal_t *connection = &bleio_connections[i];
            if (connection->conn_handle != BLE_CONN_HANDLE_INVALID && connection->user_owned) {
                common_hal_bleio_connection_disconnect(connection);
            }
        }
        nlr_pop();
    }
    // Nothing may point into the heap that is about to go.
    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        bleio_connections[i].connection_obj = mp_const_none;
        bleio_connections[i].remote_service_list = NULL;
    }
    adapter->connection_objs = NULL;
    adapter->scan_results = NULL;
    bleio_adapter_cut_attributes(adapter);
}

// After the VM (between two VMs: no heap). With the BLE workflow running and no services of the
// program's, nothing changes: the phone stays connected. Services of the program's restart the
// stack (an HCI Reset drops the phone), as on a CLUE, and the workflow starts again with the same
// handles; so does a reset with the workflow on.
void bleio_reset(void) {
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    if (workflow_running() && !vm_added_services) {
        return;
    }
    #if CIRCUITPY_BLE_FILE_SERVICE || CIRCUITPY_BLE_SERIAL_SERVICE
    const bool restart = supervisor_bluetooth_workflow_is_enabled() && adapter->persistent;
    if (restart && adapter->enabled) {
        supervisor_stop_bluetooth();
    }
    #endif

    bleio_hci_reset();

    if (adapter->persistent) {
        // Left disabled, allocated and bound; what it had on the VM's heap is gone with the heap
        // (also after an enable that failed), and enabling it again builds that anew.
        bleio_adapter_reset(adapter);
        adapter->attributes = NULL;
        adapter->scan_results = NULL;
        adapter->connection_objs = NULL;
        adapter->device_name_characteristic = NULL;
        adapter->appearance_characteristic = NULL;
        adapter->service_changed_characteristic = NULL;
        vm_used_ble = false;
        vm_added_services = false;
        #if CIRCUITPY_BLE_FILE_SERVICE || CIRCUITPY_BLE_SERIAL_SERVICE
        if (restart) {
            // Enabling may raise (a controller that does not answer): between two VMs there is no
            // one to catch it, so the workflow stays off.
            nlr_buf_t nlr;
            if (nlr_push(&nlr) == 0) {
                supervisor_start_bluetooth();
                nlr_pop();
            } else {
                mp_printf(&mp_plat_print, "BLE workflow: the controller does not answer\n");
                supervisor_bluetooth_disable_workflow();
            }
        }
        #endif
        return;
    }
    if (!common_hal_bleio_adapter_get_enabled(&common_hal_bleio_adapter_obj)) {
        return;
    }
    bleio_adapter_reset(&common_hal_bleio_adapter_obj);
    if (!vm_used_ble) {
        // No user-code BLE operations were done, so we can maintain the supervisor state.
        return;
    }
    common_hal_bleio_adapter_set_enabled(&common_hal_bleio_adapter_obj, false);
    common_hal_bleio_adapter_obj.allocated = false;

    bleio_set_adapter(mp_const_none);

    // FIX bonding_reset();
    supervisor_start_bluetooth();
}

// The singleton _bleio.Adapter object, bound to _bleio.adapter
bleio_adapter_obj_t common_hal_bleio_adapter_obj = {
    .base = {
        .type = &bleio_adapter_type,
    },
};

bleio_adapter_obj_t *common_hal_bleio_allocate_adapter_or_raise(void) {
    if (common_hal_bleio_adapter_obj.allocated) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Too many Adapters"));
    }
    return &common_hal_bleio_adapter_obj;
}

void bleio_check_connected(uint16_t conn_handle) {
    if (conn_handle == BLE_CONN_HANDLE_INVALID) {
        mp_raise_ConnectionError(MP_ERROR_TEXT("Not connected"));
    }
}

void common_hal_bleio_gc_collect(void) {
    bleio_adapter_gc_collect(&common_hal_bleio_adapter_obj);
}


void bleio_hci_background(void) {
    bleio_adapter_background(&common_hal_bleio_adapter_obj);
    if (hci_busy()) {
        // Inside a command or a send (their waits run the background): nothing else sends now.
        return;
    }
    #if CIRCUITPY_BLE_FILE_SERVICE || CIRCUITPY_BLE_SERIAL_SERVICE
    // The BLE workflow's file commands, and its advertising again after a disconnect.
    supervisor_bluetooth_background();
    #endif
    // What the packet buffers gathered in this pass, sent; not inside an ATT wait, whose answer
    // or confirmation a flush's own would take.
    if (!att_busy()) {
        bleio_packet_buffer_background();
    }
}
