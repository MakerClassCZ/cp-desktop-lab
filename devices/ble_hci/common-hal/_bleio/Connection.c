// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Artur Pacholec
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/_bleio/Connection.h"

#include "att.h"
#include "hci_include/att_internal.h"
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
#include "shared-bindings/_bleio/Descriptor.h"
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
    return 1.25f * self->conn_interval;
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

// ---- the GATT client: discovery ----

// A UUID from ATT's little-endian bytes: 2 of them, or 16.
static bleio_uuid_obj_t *uuid_from_att(const uint8_t *data, size_t len) {
    bleio_uuid_obj_t *uuid = mp_obj_malloc(bleio_uuid_obj_t, &bleio_uuid_type);
    if (len == 2) {
        common_hal_bleio_uuid_construct(uuid, data[0] | data[1] << 8, NULL);
    } else {
        common_hal_bleio_uuid_construct(uuid, data[12] | data[13] << 8, data);
    }
    return uuid;
}

// A response, or a raised error: an Error Response other than Attribute Not Found (which ends a
// range, false), no answer in time, or the link gone.
static bool discovery_rsp(bleio_connection_internal_t *connection, int len, const uint8_t *rsp, uint8_t expected) {
    if (len == 0) {
        bleio_check_connected(connection->conn_handle);
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Timeout waiting for ATT response"));
    }
    if (rsp[0] == BT_ATT_OP_ERROR_RSP && len >= 5) {
        if (rsp[4] == BT_ATT_ERR_ATTRIBUTE_NOT_FOUND) {
            return false;
        }
        att_check_error(rsp[4]);
    }
    return rsp[0] == expected;
}

static bleio_service_obj_t *new_remote_service(bleio_connection_internal_t *connection, mp_obj_list_t *found,
    uint16_t start, uint16_t end, bleio_uuid_obj_t *uuid) {
    bleio_service_obj_t *service = mp_obj_malloc(bleio_service_obj_t, &bleio_service_type);
    bleio_service_from_connection(service, bleio_connection_new_from_internal(connection));
    service->start_handle = start;
    service->end_handle = end;
    service->handle = start;
    service->uuid = uuid;
    mp_obj_list_append(MP_OBJ_FROM_PTR(found), MP_OBJ_FROM_PTR(service));
    return service;
}

// All primary services: Read By Group Type over the whole range.
static void discover_all_services(bleio_connection_internal_t *connection, mp_obj_list_t *found) {
    uint8_t rsp[ATT_CLIENT_RSP_MAX];
    for (uint32_t start = 0x0001; start <= 0xffff;) {
        int len = att_read_group_req(connection->conn_handle, start, 0xffff, BLE_UUID_SERVICE_PRIMARY, rsp, sizeof(rsp));
        if (!discovery_rsp(connection, len, rsp, BT_ATT_OP_READ_GROUP_RSP) || len < 2 || rsp[1] < 6) {
            return;
        }
        const size_t entry = rsp[1];
        uint16_t end = 0xffff;
        for (size_t i = 2; i + entry <= (size_t)len; i += entry) {
            const uint16_t s = rsp[i] | rsp[i + 1] << 8;
            end = rsp[i + 2] | rsp[i + 3] << 8;
            if (entry == 6 || entry == 20) {
                new_remote_service(connection, found, s, end, uuid_from_att(rsp + i + 4, entry - 4));
            }
        }
        if (end < start) {
            return;
        }
        start = (uint32_t)end + 1;
    }
}

// The primary services of one UUID: Find By Type Value.
static void discover_services_of(bleio_connection_internal_t *connection, mp_obj_list_t *found, bleio_uuid_obj_t *uuid) {
    uint8_t value[16];
    const size_t value_len = common_hal_bleio_uuid_get_size(uuid) / 8;
    common_hal_bleio_uuid_pack_into(uuid, value);
    uint8_t rsp[ATT_CLIENT_RSP_MAX];
    for (uint32_t start = 0x0001; start <= 0xffff;) {
        int len = att_find_type_req(connection->conn_handle, start, 0xffff, BLE_UUID_SERVICE_PRIMARY, value, value_len, rsp, sizeof(rsp));
        if (!discovery_rsp(connection, len, rsp, BT_ATT_OP_FIND_TYPE_RSP)) {
            return;
        }
        uint16_t end = 0xffff;
        for (size_t i = 1; i + 4 <= (size_t)len; i += 4) {
            const uint16_t s = rsp[i] | rsp[i + 1] << 8;
            end = rsp[i + 2] | rsp[i + 3] << 8;
            new_remote_service(connection, found, s, end, uuid);
        }
        if (len < 5 || end < start) {
            return;
        }
        start = (uint32_t)end + 1;
    }
}

// A service's characteristics: Read By Type of the declarations (0x2803) over its range.
static void discover_characteristics(bleio_connection_internal_t *connection, bleio_service_obj_t *service) {
    uint8_t rsp[ATT_CLIENT_RSP_MAX];
    mp_buffer_info_t empty = { .buf = NULL, .len = 0 };
    for (uint32_t start = service->start_handle; start <= service->end_handle;) {
        int len = att_read_type_req(connection->conn_handle, start, service->end_handle, BLE_UUID_CHARACTERISTIC, rsp, sizeof(rsp));
        if (!discovery_rsp(connection, len, rsp, BT_ATT_OP_READ_TYPE_RSP) || len < 2) {
            return;
        }
        // Each: the declaration's handle, the properties, the value's handle, the UUID.
        const size_t entry = rsp[1];
        if (entry != 7 && entry != 21) {
            return;
        }
        uint16_t last = 0;
        for (size_t i = 2; i + entry <= (size_t)len; i += entry) {
            const uint16_t decl = rsp[i] | rsp[i + 1] << 8;
            const uint8_t props = rsp[i + 2];
            const uint16_t value = rsp[i + 3] | rsp[i + 4] << 8;
            bleio_characteristic_obj_t *characteristic = mp_obj_malloc(bleio_characteristic_obj_t, &bleio_characteristic_type);
            common_hal_bleio_characteristic_construct(characteristic, service, value,
                uuid_from_att(rsp + i + 5, entry - 5), att_properties_to_bleio(props),
                SECURITY_MODE_OPEN, SECURITY_MODE_OPEN, BLE_GATTS_VAR_ATTR_LEN_MAX, false, &empty, NULL);
            characteristic->decl_handle = decl;
            mp_obj_list_append(MP_OBJ_FROM_PTR(service->characteristic_list), MP_OBJ_FROM_PTR(characteristic));
            last = value;
        }
        if (last < start) {
            return;
        }
        start = (uint32_t)last + 1;
    }
}

// A characteristic's descriptors: Find Information between its value and the next declaration
// (or the service's end). The CCCD is kept for set_cccd().
static void discover_descriptors(bleio_connection_internal_t *connection, bleio_characteristic_obj_t *characteristic, uint16_t end) {
    uint8_t rsp[ATT_CLIENT_RSP_MAX];
    mp_buffer_info_t empty = { .buf = NULL, .len = 0 };
    for (uint32_t start = characteristic->handle + 1; start <= end;) {
        int len = att_find_info_req(connection->conn_handle, start, end, rsp, sizeof(rsp));
        if (!discovery_rsp(connection, len, rsp, BT_ATT_OP_FIND_INFO_RSP) || len < 2) {
            return;
        }
        // Format 1: 16-bit UUIDs, each with its handle; 2: 128-bit ones.
        const size_t uuid_len = rsp[1] == BT_ATT_INFO_16 ? 2 : 16;
        uint16_t last = 0;
        for (size_t i = 2; i + 2 + uuid_len <= (size_t)len; i += 2 + uuid_len) {
            const uint16_t handle = rsp[i] | rsp[i + 1] << 8;
            last = handle;
            bleio_descriptor_obj_t *descriptor = mp_obj_malloc(bleio_descriptor_obj_t, &bleio_descriptor_type);
            bleio_uuid_obj_t *uuid = uuid_from_att(rsp + i + 2, uuid_len);
            common_hal_bleio_descriptor_construct(descriptor, characteristic, uuid,
                SECURITY_MODE_OPEN, SECURITY_MODE_OPEN, GATT_MAX_DATA_LENGTH, false, &empty);
            descriptor->handle = handle;
            switch (bleio_uuid_get_uuid16_or_unknown(uuid)) {
                case BLE_UUID_CCCD:
                    characteristic->cccd = descriptor;
                    break;
                case BLE_UUID_SCCD:
                    characteristic->sccd = descriptor;
                    break;
                case BLE_UUID_CHAR_USER_DESC:
                    characteristic->user_desc = descriptor;
                    break;
            }
            mp_obj_list_append(MP_OBJ_FROM_PTR(characteristic->descriptor_list), MP_OBJ_FROM_PTR(descriptor));
        }
        if (last < start) {
            return;
        }
        start = (uint32_t)last + 1;
    }
}

static mp_obj_list_t *discover_remote_services(bleio_connection_internal_t *self, mp_obj_t service_uuids_whitelist) {
    // The MTU is exchanged once, at the first discovery, as a client starts: both sides then use
    // the smaller of the two.
    if (!self->mtu_exchanged) {
        self->mtu_exchanged = true;
        att_exchange_mtu(self->conn_handle);
    }
    mp_obj_list_t *found = MP_OBJ_TO_PTR(mp_obj_new_list(0, NULL));
    if (service_uuids_whitelist == mp_const_none) {
        discover_all_services(self, found);
    } else {
        mp_obj_iter_buf_t iter_buf;
        mp_obj_t iterable = mp_getiter(service_uuids_whitelist, &iter_buf);
        mp_obj_t uuid_obj;
        while ((uuid_obj = mp_iternext(iterable)) != MP_OBJ_STOP_ITERATION) {
            if (!mp_obj_is_type(uuid_obj, &bleio_uuid_type)) {
                mp_raise_TypeError(MP_ERROR_TEXT("non-UUID found in service_uuids_whitelist"));
            }
            discover_services_of(self, found, MP_OBJ_TO_PTR(uuid_obj));
        }
    }
    for (size_t i = 0; i < found->len; i++) {
        bleio_service_obj_t *service = MP_OBJ_TO_PTR(found->items[i]);
        discover_characteristics(self, service);
        const size_t count = service->characteristic_list->len;
        for (size_t j = 0; j < count; j++) {
            bleio_characteristic_obj_t *characteristic = MP_OBJ_TO_PTR(service->characteristic_list->items[j]);
            const uint16_t end = j + 1 < count
                ? ((bleio_characteristic_obj_t *)MP_OBJ_TO_PTR(service->characteristic_list->items[j + 1]))->decl_handle - 1
                : service->end_handle;
            discover_descriptors(self, characteristic, end);
        }
    }

    // The services found join the ones the connection knows, where notifications and indications
    // find their characteristics (att.c): a program discovers one service and then another
    // (adafruit_ble's connection[Service]), and the first keeps its notifications. A service
    // found again replaces the one it was.
    if (self->remote_service_list == NULL) {
        self->remote_service_list = MP_OBJ_TO_PTR(mp_obj_new_list(0, NULL));
    }
    mp_obj_list_t *known = self->remote_service_list;
    for (size_t i = 0; i < found->len; i++) {
        const bleio_service_obj_t *service = MP_OBJ_TO_PTR(found->items[i]);
        size_t j = 0;
        while (j < known->len &&
               ((bleio_service_obj_t *)MP_OBJ_TO_PTR(known->items[j]))->start_handle != service->start_handle) {
            j++;
        }
        if (j < known->len) {
            known->items[j] = found->items[i];
        } else {
            mp_obj_list_append(MP_OBJ_FROM_PTR(known), found->items[i]);
        }
    }
    return found;
}

mp_obj_tuple_t *common_hal_bleio_connection_discover_remote_services(bleio_connection_obj_t *self, mp_obj_t service_uuids_whitelist) {
    bleio_connection_ensure_connected(self);
    // The lists are made here, on the program's heap, as nordic makes them: never in the event
    // handler that adds the connection, which may run between two VMs.
    mp_obj_list_t *found = discover_remote_services(self->connection, service_uuids_whitelist);
    return mp_obj_new_tuple(found->len, found->items);
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
