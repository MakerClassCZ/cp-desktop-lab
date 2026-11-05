// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Artur Pacholec
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/_bleio/Connection.h"

#include "att.h"
#include "smp.h"

#include <string.h>
#include <stdio.h>

#include "shared/runtime/interrupt_char.h"
#include "py/gc.h"
#include "py/objlist.h"
#include "py/objstr.h"
#include "py/qstr.h"
#include "py/runtime.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Adapter.h"
#include "shared-bindings/_bleio/Attribute.h"
#include "shared-bindings/_bleio/Characteristic.h"
#include "shared-bindings/_bleio/Service.h"
#include "shared-bindings/_bleio/UUID.h"
#include "supervisor/shared/tick.h"

#define BLE_ADV_LENGTH_FIELD_SIZE   1
#define BLE_ADV_AD_TYPE_FIELD_SIZE  1
#define BLE_AD_TYPE_FLAGS_DATA_SIZE 1

#define CONNECTION_DEBUG (1)
#if CONNECTION_DEBUG
  #define CONNECTION_DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
  #define CONNECTION_DEBUG_PRINTF(...)
#endif

static volatile bool m_discovery_in_process;
static volatile bool m_discovery_successful;

// FIX static bleio_service_obj_t *m_char_discovery_service;
// FIX static bleio_characteristic_obj_t *m_desc_discovery_characteristic;

void bleio_connection_clear(bleio_connection_internal_t *self) {
    if (self->remote_service_list != NULL) {
        mp_obj_list_clear(MP_OBJ_FROM_PTR(self->remote_service_list));
    }

    self->conn_handle = BLE_CONN_HANDLE_INVALID;
    self->pair_status = PAIR_NOT_PAIRED;
    self->is_central = false;
}

// Paired: the link is encrypted, by a pairing or with a bond's key (as nordic's PAIR_PAIRED).
bool common_hal_bleio_connection_get_paired(bleio_connection_obj_t *self) {
    if (self->connection == NULL) {
        return false;
    }
    return self->connection->conn_handle != BLE_CONN_HANDLE_INVALID && self->connection->encrypted;
}

bool common_hal_bleio_connection_get_connected(bleio_connection_obj_t *self) {
    if (self->connection == NULL) {
        return false;
    }
    return self->connection->conn_handle != BLE_CONN_HANDLE_INVALID;
}

// "Just Works" keys are not MITM-protected: never authenticated.
bool common_hal_bleio_connection_get_authenticated(bleio_connection_obj_t *self) {
    return false;
}

mp_obj_t common_hal_bleio_connection_get_numeric_comparison(bleio_connection_obj_t *self) {
    return mp_const_none;
}

void common_hal_bleio_connection_confirm_pairing(bleio_connection_obj_t *self, bool accept) {
    // There is no numeric comparison without LE Secure Connections: nothing is ever pending.
    mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("No pairing in progress"));
}

void common_hal_bleio_connection_disconnect(bleio_connection_internal_t *self) {
    hci_disconnect(self->conn_handle);
}

void common_hal_bleio_connection_pair(bleio_connection_internal_t *self, bool bond) {
    smp_pair(self, bond);
}

mp_float_t common_hal_bleio_connection_get_connection_interval(bleio_connection_internal_t *self) {
    while (self->conn_params_updating && !mp_hal_is_interrupted()) {
        RUN_BACKGROUND_TASKS;
    }
    // FIX return 1.25f * self->conn_params.min_conn_interval;
    return 0.0f;
}

// Return the current negotiated MTU length, minus overhead.
mp_int_t common_hal_bleio_connection_get_max_packet_length(bleio_connection_internal_t *self) {
    return (self->mtu == 0 ? BT_ATT_DEFAULT_LE_MTU : self->mtu) - 3;
}

void common_hal_bleio_connection_set_connection_interval(bleio_connection_internal_t *self, mp_float_t new_interval) {
    // self->conn_params_updating = true;
    // uint16_t interval = new_interval / 1.25f;
    // self->conn_params.min_conn_interval = interval;
    // self->conn_params.max_conn_interval = interval;
    // uint32_t status = NRF_ERROR_BUSY;
    // while (status == NRF_ERROR_BUSY) {
    //     status = sd_ble_gap_conn_param_update(self->conn_handle, &self->conn_params);
    //     RUN_BACKGROUND_TASKS;
    // }
    // check_nrf_error(status);
}

// service_uuid may be NULL, to discover all services.
// static bool discover_next_services(bleio_connection_internal_t* connection, uint16_t start_handle, ble_uuid_t *service_uuid) {
//     m_discovery_successful = false;
//     m_discovery_in_process = true;

//     uint32_t nrf_err = NRF_ERROR_BUSY;
//     while (nrf_err == NRF_ERROR_BUSY) {
//         nrf_err = sd_ble_gattc_primary_services_discover(connection->conn_handle, start_handle, service_uuid);
//     }
//     check_nrf_error(nrf_err);

//     // Wait for a discovery event.
//     while (m_discovery_in_process) {
//         MICROPY_VM_HOOK_LOOP;
//     }
//     return m_discovery_successful;
// }

// static bool discover_next_characteristics(bleio_connection_internal_t* connection, bleio_service_obj_t *service, uint16_t start_handle) {
//     m_char_discovery_service = service;

//     ble_gattc_handle_range_t handle_range;
//     handle_range.start_handle = start_handle;
//     handle_range.end_handle = service->end_handle;

//     m_discovery_successful = false;
//     m_discovery_in_process = true;

//     uint32_t err_code = sd_ble_gattc_characteristics_discover(connection->conn_handle, &handle_range);
//     if (err_code != NRF_SUCCESS) {
//         return false;
//     }

//     // Wait for a discovery event.
//     while (m_discovery_in_process) {
//         MICROPY_VM_HOOK_LOOP;
//     }
//     return m_discovery_successful;
// }

// static bool discover_next_descriptors(bleio_connection_internal_t* connection, bleio_characteristic_obj_t *characteristic, uint16_t start_handle, uint16_t end_handle) {
//     m_desc_discovery_characteristic = characteristic;

//     ble_gattc_handle_range_t handle_range;
//     handle_range.start_handle = start_handle;
//     handle_range.end_handle = end_handle;

//     m_discovery_successful = false;
//     m_discovery_in_process = true;

//     uint32_t err_code = sd_ble_gattc_descriptors_discover(connection->conn_handle, &handle_range);
//     if (err_code != NRF_SUCCESS) {
//         return false;
//     }

//     // Wait for a discovery event.
//     while (m_discovery_in_process) {
//         MICROPY_VM_HOOK_LOOP;
//     }
//     return m_discovery_successful;
// }

// static void on_primary_srv_discovery_rsp(ble_gattc_evt_prim_srvc_disc_rsp_t *response, bleio_connection_internal_t* connection) {
//     for (size_t i = 0; i < response->count; ++i) {
//         ble_gattc_service_t *gattc_service = &response->services[i];

//         bleio_service_obj_t *service = mp_obj_malloc(bleio_service_obj_t, &bleio_service_type);

//         // Initialize several fields at once.
//         bleio_service_from_connection(service, bleio_connection_new_from_internal(connection));

//         service->is_remote = true;
//         service->start_handle = gattc_service->handle_range.start_handle;
//         service->end_handle = gattc_service->handle_range.end_handle;
//         service->handle = gattc_service->handle_range.start_handle;

//         if (gattc_service->uuid.type != BLE_UUID_TYPE_UNKNOWN) {
//             // Known service UUID.
//             bleio_uuid_obj_t *uuid = mp_obj_malloc(bleio_uuid_obj_t, &bleio_uuid_type);
//             bleio_uuid_construct_from_nrf_ble_uuid(uuid, &gattc_service->uuid);
//             service->uuid = uuid;
//         } else {
//             // The discovery response contained a 128-bit UUID that has not yet been registered with the
//             // softdevice via sd_ble_uuid_vs_add(). We need to fetch the 128-bit value and register it.
//             // For now, just set the UUID to NULL.
//             service->uuid = NULL;
//         }
//
//         mp_obj_list_append(MP_OBJ_FROM_PTR(connection->remote_service_list),
//                            MP_OBJ_FROM_PTR(service));
//     }
//
//     if (response->count > 0) {
//         m_discovery_successful = true;
//     }
//     m_discovery_in_process = false;
// }

// static void on_char_discovery_rsp(ble_gattc_evt_char_disc_rsp_t *response, bleio_connection_internal_t* connection) {
//     for (size_t i = 0; i < response->count; ++i) {
//         ble_gattc_char_t *gattc_char = &response->chars[i];

//         bleio_characteristic_obj_t *characteristic =
//             mp_obj_malloc(bleio_characteristic_obj_t, &bleio_characteristic_type);

//         bleio_uuid_obj_t *uuid = NULL;

//         if (gattc_char->uuid.type != BLE_UUID_TYPE_UNKNOWN) {
//             // Known characteristic UUID.
//             uuid = mp_obj_malloc(bleio_uuid_obj_t, &bleio_uuid_type);
//             bleio_uuid_construct_from_nrf_ble_uuid(uuid, &gattc_char->uuid);
//         } else {
//             // The discovery response contained a 128-bit UUID that has not yet been registered with the
//             // softdevice via sd_ble_uuid_vs_add(). We need to fetch the 128-bit value and register it.
//             // For now, just leave the UUID as NULL.
//         }

//         bleio_characteristic_properties_t props =
//             (gattc_char->char_props.broadcast ? CHAR_PROP_BROADCAST : 0) |
//             (gattc_char->char_props.indicate ? CHAR_PROP_INDICATE : 0) |
//             (gattc_char->char_props.notify ? CHAR_PROP_NOTIFY : 0) |
//             (gattc_char->char_props.read ? CHAR_PROP_READ : 0) |
//             (gattc_char->char_props.write ? CHAR_PROP_WRITE : 0) |
//             (gattc_char->char_props.write_wo_resp ? CHAR_PROP_WRITE_NO_RESPONSE : 0);

//         // Call common_hal_bleio_characteristic_construct() to initialize some fields and set up evt handler.
//         common_hal_bleio_characteristic_construct(
//             characteristic, m_char_discovery_service, gattc_char->handle_value, uuid,
//             props, SECURITY_MODE_OPEN, SECURITY_MODE_OPEN,
//             GATT_MAX_DATA_LENGTH, false,   // max_length, fixed_length: values may not matter for gattc
//             NULL);

//         mp_obj_list_append(MP_OBJ_FROM_PTR(m_char_discovery_service->characteristic_list),
//                            MP_OBJ_FROM_PTR(characteristic));
//     }

//     if (response->count > 0) {
//         m_discovery_successful = true;
//     }
//     m_discovery_in_process = false;
// }

// static void on_desc_discovery_rsp(ble_gattc_evt_desc_disc_rsp_t *response, bleio_connection_internal_t* connection) {
//     for (size_t i = 0; i < response->count; ++i) {
//         ble_gattc_desc_t *gattc_desc = &response->descs[i];

//         // Remember handles for certain well-known descriptors.
//         switch (gattc_desc->uuid.uuid) {
//             case BLE_UUID_DESCRIPTOR_CLIENT_CHAR_CONFIG:
//                 m_desc_discovery_characteristic->cccd_handle = gattc_desc->handle;
//                 break;

//             case BLE_UUID_DESCRIPTOR_SERVER_CHAR_CONFIG:
//                 m_desc_discovery_characteristic->sccd_handle = gattc_desc->handle;
//                 break;

//             case BLE_UUID_DESCRIPTOR_CHAR_USER_DESC:
//                 m_desc_discovery_characteristic->user_desc_handle = gattc_desc->handle;
//                 break;

//             default:
//                 // TODO: sd_ble_gattc_descriptors_discover() can return things that are not descriptors,
//                 // so ignore those.
//                 // https://devzone.nordicsemi.com/f/nordic-q-a/49500/sd_ble_gattc_descriptors_discover-is-returning-attributes-that-are-not-descriptors
//                 break;
//         }

//         bleio_descriptor_obj_t *descriptor = mp_obj_malloc(bleio_descriptor_obj_t, &bleio_descriptor_type);

//         bleio_uuid_obj_t *uuid = NULL;

//         if (gattc_desc->uuid.type != BLE_UUID_TYPE_UNKNOWN) {
//             // Known descriptor UUID.
//             uuid = mp_obj_malloc(bleio_uuid_obj_t, &bleio_uuid_type);
//             bleio_uuid_construct_from_nrf_ble_uuid(uuid, &gattc_desc->uuid);
//         } else {
//             // The discovery response contained a 128-bit UUID that has not yet been registered with the
//             // softdevice via sd_ble_uuid_vs_add(). We need to fetch the 128-bit value and register it.
//             // For now, just leave the UUID as NULL.
//         }

//         common_hal_bleio_descriptor_construct(
//             descriptor, m_desc_discovery_characteristic, uuid,
//             SECURITY_MODE_OPEN, SECURITY_MODE_OPEN,
//             GATT_MAX_DATA_LENGTH, false, mp_const_empty_bytes);
//         descriptor->handle = gattc_desc->handle;

//         mp_obj_list_append(MP_OBJ_FROM_PTR(m_desc_discovery_characteristic->descriptor_list),
//                            MP_OBJ_FROM_PTR(descriptor));
//     }

//     if (response->count > 0) {
//         m_discovery_successful = true;
//     }
//     m_discovery_in_process = false;
// }

// static bool discovery_on_ble_evt(ble_evt_t *ble_evt, mp_obj_t payload) {
//     bleio_connection_internal_t* connection = MP_OBJ_TO_PTR(payload);
//     switch (ble_evt->header.evt_id) {
//         case BLE_GAP_EVT_DISCONNECTED:
//             m_discovery_successful = false;
//             m_discovery_in_process = false;
//             break;

//         case BLE_GATTC_EVT_PRIM_SRVC_DISC_RSP:
//             on_primary_srv_discovery_rsp(&ble_evt->evt.gattc_evt.params.prim_srvc_disc_rsp, connection);
//             break;

//         case BLE_GATTC_EVT_CHAR_DISC_RSP:
//             on_char_discovery_rsp(&ble_evt->evt.gattc_evt.params.char_disc_rsp, connection);
//             break;

//         case BLE_GATTC_EVT_DESC_DISC_RSP:
//             on_desc_discovery_rsp(&ble_evt->evt.gattc_evt.params.desc_disc_rsp, connection);
//             break;

//         default:
//             // CONNECTION_DEBUG_PRINTF(&mp_plat_print, "Unhandled discovery event: 0x%04x\n", ble_evt->header.evt_id);
//             return false;
//             break;
//     }
//     return true;
// }

// static void discover_remote_services(bleio_connection_internal_t *self, mp_obj_t service_uuids_whitelist) {
//     ble_drv_add_event_handler(discovery_on_ble_evt, self);

//     // Start over with an empty list.
//     self->remote_service_list = mp_obj_new_list(0, NULL);


//     if (service_uuids_whitelist == mp_const_none) {
//         // List of service UUID's not given, so discover all available services.

//         uint16_t next_service_start_handle = BLE_GATT_HANDLE_START;

//         while (discover_next_services(self, next_service_start_handle, MP_OBJ_NULL)) {
//             // discover_next_services() appends to remote_services_list.

//             // Get the most recently discovered service, and then ask for services
//             // whose handles start after the last attribute handle inside that service.
//             // There must be at least one if discover_next_services() returned true.
//             const bleio_service_obj_t *service =
//                 self->remote_service_list->items[self->remote_service_list->len - 1];
//             next_service_start_handle = service->end_handle + 1;
//         }
//     } else {
//         mp_obj_iter_buf_t iter_buf;
//         mp_obj_t iterable = mp_getiter(service_uuids_whitelist, &iter_buf);
//         mp_obj_t uuid_obj;
//         while ((uuid_obj = mp_iternext(iterable)) != MP_OBJ_STOP_ITERATION) {
//             if (!mp_obj_is_type(uuid_obj, &bleio_uuid_type)) {
//                 mp_raise_TypeError(MP_ERROR_TEXT("non-UUID found in service_uuids_whitelist"));
//             }
//             bleio_uuid_obj_t *uuid = MP_OBJ_TO_PTR(uuid_obj);

//             ble_uuid_t nrf_uuid;
//             bleio_uuid_convert_to_nrf_ble_uuid(uuid, &nrf_uuid);

//             // Service might or might not be discovered; that's ok. Caller has to check
//             // Central.remote_services to find out.
//             // We only need to call this once for each service to discover.
//             discover_next_services(self, BLE_GATT_HANDLE_START, &nrf_uuid);
//         }
//     }


//     for (size_t i = 0; i < self->remote_service_list->len; i++) {
//         bleio_service_obj_t *service = MP_OBJ_TO_PTR(self->remote_service_list->items[i]);
//         // Skip the service if it had an unknown (unregistered) UUID.
//         if (service->uuid == NULL) {
//             continue;
//         }

//         uint16_t next_char_start_handle = service->start_handle;

//         // Stop when we go past the end of the range of handles for this service or
//         // discovery call returns nothing.
//         // discover_next_characteristics() appends to the characteristic_list.
//         while (next_char_start_handle <= service->end_handle &&
//                discover_next_characteristics(self, service, next_char_start_handle)) {


//             // Get the most recently discovered characteristic, and then ask for characteristics
//             // whose handles start after the last attribute handle inside that characteristic.
//             const bleio_characteristic_obj_t *characteristic =
//                 MP_OBJ_TO_PTR(service->characteristic_list->items[service->characteristic_list->len - 1]);

//             next_char_start_handle = characteristic->handle + 1;
//         }

//         // Got characteristics for this service. Now discover descriptors for each characteristic.
//         size_t char_list_len = service->characteristic_list->len;
//         for (size_t char_idx = 0; char_idx < char_list_len; ++char_idx) {
//             bleio_characteristic_obj_t *characteristic =
//                 MP_OBJ_TO_PTR(service->characteristic_list->items[char_idx]);
//             const bool last_characteristic = char_idx == char_list_len - 1;
//             bleio_characteristic_obj_t *next_characteristic = last_characteristic
//                 ? NULL
//                 : MP_OBJ_TO_PTR(service->characteristic_list->items[char_idx + 1]);

//             // Skip the characteristic if it had an unknown (unregistered) UUID.
//             if (characteristic->uuid == NULL) {
//                 continue;
//             }

//             uint16_t next_desc_start_handle = characteristic->handle + 1;

//             // Don't run past the end of this service or the beginning of the next characteristic.
//             uint16_t next_desc_end_handle = next_characteristic == NULL
//                 ? service->end_handle
//                 : next_characteristic->handle - 1;

//             // Stop when we go past the end of the range of handles for this service or
//             // discovery call returns nothing.
//             // discover_next_descriptors() appends to the descriptor_linked_list.
//             while (next_desc_start_handle <= service->end_handle &&
//                    next_desc_start_handle <= next_desc_end_handle &&
//                    discover_next_descriptors(self, characteristic,
//                                              next_desc_start_handle, next_desc_end_handle)) {
//                 // Get the most recently discovered descriptor, and then ask for descriptors
//                 // whose handles start after that descriptor's handle.
//                 // There must be at least one if discover_next_descriptors() returned true.
//                 const bleio_descriptor_obj_t *descriptor =
//                     characteristic->descriptor_list->items[characteristic->descriptor_list->len - 1];
//                 next_desc_start_handle = descriptor->handle + 1;
//             }
//         }
//     }

//     // This event handler is no longer needed.
//     ble_drv_remove_event_handler(discovery_on_ble_evt, self);

// }

mp_obj_tuple_t *common_hal_bleio_connection_discover_remote_services(bleio_connection_obj_t *self, mp_obj_t service_uuids_whitelist) {
    // FIX discover_remote_services(self->connection, service_uuids_whitelist);
    bleio_connection_ensure_connected(self);
    // The list is made here, on the program's heap, as nordic makes it: never in the event
    // handler that adds the connection, which may run between two VMs.
    if (self->connection->remote_service_list == NULL) {
        self->connection->remote_service_list = mp_obj_new_list(0, NULL);
    }
    // Convert to a tuple and then clear the list so the callee will take ownership.
    mp_obj_tuple_t *services_tuple =
        mp_obj_new_tuple(self->connection->remote_service_list->len,
            self->connection->remote_service_list->items);
    mp_obj_list_clear(MP_OBJ_FROM_PTR(self->connection->remote_service_list));
    return services_tuple;
}

uint16_t bleio_connection_get_conn_handle(bleio_connection_obj_t *self) {
    if (self == NULL || self->connection == NULL) {
        return BLE_CONN_HANDLE_INVALID;
    }
    return self->connection->conn_handle;
}

mp_obj_t bleio_connection_new_from_internal(bleio_connection_internal_t *internal) {
    if (internal->connection_obj != mp_const_none) {
        return internal->connection_obj;
    }
    bleio_connection_obj_t *connection = mp_obj_malloc(bleio_connection_obj_t, &bleio_connection_type);
    connection->connection = internal;
    internal->connection_obj = connection;

    return MP_OBJ_FROM_PTR(connection);
}

// Find the connection that uses the given conn_handle. Return NULL if not found.
bleio_connection_internal_t *bleio_conn_handle_to_connection(uint16_t conn_handle) {
    bleio_connection_internal_t *connection;
    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        connection = &bleio_connections[i];
        if (connection->conn_handle == conn_handle) {
            return connection;
        }
    }

    return NULL;
}
