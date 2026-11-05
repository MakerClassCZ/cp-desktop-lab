// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Artur Pacholec
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"

#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Characteristic.h"
#include "shared-bindings/_bleio/CharacteristicBuffer.h"
#include "shared-bindings/_bleio/Descriptor.h"
#include "shared-bindings/_bleio/PacketBuffer.h"
#include "shared-bindings/_bleio/Service.h"

#include "common-hal/_bleio/Adapter.h"
#include "common-hal/_bleio/att.h"

#define CCCD_NOTIFY   0x1
#define CCCD_INDICATE 0x2


void common_hal_bleio_characteristic_construct(bleio_characteristic_obj_t *self, bleio_service_obj_t *service, uint16_t handle, bleio_uuid_obj_t *uuid, bleio_characteristic_properties_t props, bleio_attribute_security_mode_t read_perm, bleio_attribute_security_mode_t write_perm, mp_int_t max_length, bool fixed_length, mp_buffer_info_t *initial_value_bufinfo, const char *user_description) {
    self->service = service;
    self->uuid = uuid;
    self->decl_handle = BLE_GATT_HANDLE_INVALID;
    self->handle = BLE_GATT_HANDLE_INVALID;
    self->props = props;
    self->read_perm = read_perm;
    self->write_perm = write_perm;
    self->observer = mp_const_none;
    self->user_desc = NULL;
    self->cccd = NULL;
    self->sccd = NULL;

    const mp_int_t max_length_max = 512;

    mp_arg_validate_int_range(max_length, 0, max_length_max, MP_QSTR_max_length);

    self->max_length = max_length;
    self->fixed_length = fixed_length;

    // Off the VM heap for _bleio's own services and the BLE workflow's (Adapter.h). The value has
    // room for max_length bytes and is written in place; the supervisor gives no initial value.
    self->descriptor_list = bleio_attribute_list_new(self, 2);
    self->value = bleio_attribute_value_new(self, service->is_remote ? 0 : max_length, initial_value_bufinfo);

    if (service->is_remote) {
        self->handle = handle;
    } else {
        common_hal_bleio_service_add_characteristic(self->service, self, initial_value_bufinfo, user_description);
    }
}

bool common_hal_bleio_characteristic_deinited(bleio_characteristic_obj_t *self) {
    return self->handle == BLE_GATT_HANDLE_INVALID;
}

void common_hal_bleio_characteristic_deinit(bleio_characteristic_obj_t *self) {
    if (common_hal_bleio_characteristic_deinited(self)) {
        return;
    }
    self->handle = BLE_GATT_HANDLE_INVALID;
}

mp_obj_tuple_t *common_hal_bleio_characteristic_get_descriptors(bleio_characteristic_obj_t *self) {
    return mp_obj_new_tuple(self->descriptor_list->len, self->descriptor_list->items);
}

bleio_service_obj_t *common_hal_bleio_characteristic_get_service(bleio_characteristic_obj_t *self) {
    return self->service;
}

size_t common_hal_bleio_characteristic_get_max_length(bleio_characteristic_obj_t *self) {
    return self->max_length;
}

size_t common_hal_bleio_characteristic_get_value(bleio_characteristic_obj_t *self, uint8_t *buf, size_t len) {
    // Do GATT operations only if this characteristic has been added to a registered service.
    if (self->handle != BLE_GATT_HANDLE_INVALID) {
        if (common_hal_bleio_service_get_is_remote(self->service)) {
            const uint16_t conn_handle = bleio_connection_get_conn_handle(self->service->connection);
            bleio_check_connected(conn_handle);
            return att_read_value(conn_handle, self->handle, buf, len);
        } else {
            mp_buffer_info_t bufinfo;
            if (!mp_get_buffer(self->value, &bufinfo, MP_BUFFER_READ)) {
                return 0;
            }
            const size_t actual_length = MIN(len, bufinfo.len);
            memcpy(buf, bufinfo.buf, actual_length);
            return actual_length;
        }
    }

    return 0;
}

void common_hal_bleio_characteristic_set_value(bleio_characteristic_obj_t *self, mp_buffer_info_t *bufinfo) {
    if (self->fixed_length && bufinfo->len != self->max_length) {
        mp_raise_ValueError(MP_ERROR_TEXT("Value length != required fixed length"));
    }
    if (bufinfo->len > self->max_length) {
        mp_raise_ValueError(MP_ERROR_TEXT("Value length > max_length"));
    }

    // Do GATT operations only if this characteristic has been added to a registered service.
    if (self->handle != BLE_GATT_HANDLE_INVALID) {
        if (common_hal_bleio_service_get_is_remote(self->service)) {
            const uint16_t conn_handle = bleio_connection_get_conn_handle(self->service->connection);
            // Without response when the characteristic takes that, as the nRF port writes.
            if (self->props & (CHAR_PROP_WRITE | CHAR_PROP_WRITE_NO_RESPONSE)) {
                bleio_check_connected(conn_handle);
                att_write_value(conn_handle, self->handle, bufinfo->buf, bufinfo->len,
                    !(self->props & CHAR_PROP_WRITE_NO_RESPONSE));
            } else {
                mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Characteristic not writable"));
            }
        } else {
            // Always write the value locally even if no connections are active. Only the value: an
            // observer (a PacketBuffer) takes what a peer writes, not what it sends itself.
            bleio_attribute_value_set(self->value, bufinfo->buf, bufinfo->len);
            // Notify or indicate all active connections.

            uint16_t cccd_value = 0;
            mp_buffer_info_t cccd_bufinfo = {
                .buf = &cccd_value,
                .len = sizeof(cccd_value),
            };

            const bool notify = self->props & CHAR_PROP_NOTIFY;
            const bool indicate = self->props & CHAR_PROP_INDICATE;
            // Read the CCCD value, if there is one.
            if ((notify | indicate) && self->cccd != NULL) {
                common_hal_bleio_descriptor_get_value(self->cccd, cccd_bufinfo.buf, cccd_bufinfo.len);
            }

            // It's possible that both notify and indicate are set.
            if (notify && (cccd_value & CCCD_NOTIFY)) {
                att_notify(self->handle, bufinfo->buf, MIN(bufinfo->len, self->max_length));
            }
            if (indicate && (cccd_value & CCCD_INDICATE)) {
                att_indicate(self->handle, bufinfo->buf, MIN(bufinfo->len, self->max_length));

            }
        }
    }
}

bleio_uuid_obj_t *common_hal_bleio_characteristic_get_uuid(bleio_characteristic_obj_t *self) {
    return self->uuid;
}

bleio_characteristic_properties_t common_hal_bleio_characteristic_get_properties(bleio_characteristic_obj_t *self) {
    return self->props;
}

void common_hal_bleio_characteristic_add_descriptor(bleio_characteristic_obj_t *self, bleio_descriptor_obj_t *descriptor) {
    if (self->handle != common_hal_bleio_adapter_obj.last_added_characteristic_handle) {
        mp_raise_bleio_BluetoothError(
            MP_ERROR_TEXT("Descriptor can only be added to most recently added characteristic"));
    }

    descriptor->handle = bleio_adapter_add_attribute(&common_hal_bleio_adapter_obj, MP_OBJ_TO_PTR(descriptor));
    // Include this descriptor in the service handle's range.
    self->service->end_handle = descriptor->handle;

    mp_obj_list_append(MP_OBJ_FROM_PTR(self->descriptor_list),
        MP_OBJ_FROM_PTR(descriptor));
}

void common_hal_bleio_characteristic_set_cccd(bleio_characteristic_obj_t *self, bool notify, bool indicate) {
    if (self->cccd == NULL) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("No CCCD for this Characteristic"));
    }

    if (!common_hal_bleio_service_get_is_remote(self->service)) {
        mp_raise_bleio_RoleError(MP_ERROR_TEXT("Can't set CCCD on local Characteristic"));
    }

    const uint16_t conn_handle = bleio_connection_get_conn_handle(self->service->connection);
    bleio_check_connected(conn_handle);

    uint16_t cccd_value =
        (notify ? CCCD_NOTIFY : 0) |
        (indicate ? CCCD_INDICATE : 0);

    // Little-endian, as ATT carries it.
    const uint8_t value[2] = { cccd_value & 0xff, cccd_value >> 8 };
    att_write_value(conn_handle, self->cccd->handle, value, sizeof(value), true);
}

// A peer wrote the value: the observer gets it too.
bool bleio_characteristic_set_local_value(bleio_characteristic_obj_t *self, mp_buffer_info_t *bufinfo) {
    if (self->fixed_length && bufinfo->len != self->max_length) {
        return false;
    }
    if (bufinfo->len > self->max_length) {
        return false;
    }

    bleio_attribute_value_set(self->value, bufinfo->buf, bufinfo->len);

    if (mp_obj_is_type(self->observer, &bleio_characteristic_buffer_type)) {
        bleio_characteristic_buffer_update(MP_OBJ_FROM_PTR(self->observer), bufinfo);
    } else if (mp_obj_is_type(self->observer, &bleio_packet_buffer_type)) {
        bleio_packet_buffer_update(MP_OBJ_FROM_PTR(self->observer), bufinfo);
    } else {
        return false;
    }
    return true;
}

void bleio_characteristic_peer(bleio_characteristic_obj_t *self, uint16_t conn_handle) {
    if (mp_obj_is_type(self->observer, &bleio_packet_buffer_type)) {
        bleio_packet_buffer_peer(MP_OBJ_TO_PTR(self->observer), conn_handle);
    }
}

void bleio_characteristic_set_observer(bleio_characteristic_obj_t *self, mp_obj_t observer) {
    self->observer = observer;
}

void bleio_characteristic_clear_observer(bleio_characteristic_obj_t *self) {
    self->observer = mp_const_none;
}
