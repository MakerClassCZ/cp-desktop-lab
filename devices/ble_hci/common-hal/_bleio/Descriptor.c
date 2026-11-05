// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Artur Pacholec
// SPDX-FileCopyrightText: Copyright (c) 2016 Glenn Ruben Bakke
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"

#include "att.h"

#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Descriptor.h"
#include "shared-bindings/_bleio/Service.h"
#include "shared-bindings/_bleio/UUID.h"
#include "common-hal/_bleio/Adapter.h"

void common_hal_bleio_descriptor_construct(bleio_descriptor_obj_t *self, bleio_characteristic_obj_t *characteristic, bleio_uuid_obj_t *uuid, bleio_attribute_security_mode_t read_perm, bleio_attribute_security_mode_t write_perm, mp_int_t max_length, bool fixed_length, mp_buffer_info_t *initial_value_bufinfo) {
    self->characteristic = characteristic;
    self->uuid = uuid;
    self->handle = BLE_GATT_HANDLE_INVALID;
    self->read_perm = read_perm;
    self->write_perm = write_perm;

    const mp_int_t max_length_max = fixed_length ? BLE_GATTS_FIX_ATTR_LEN_MAX : BLE_GATTS_VAR_ATTR_LEN_MAX;
    if (max_length < 0 || max_length > max_length_max) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("max_length must be 0-%d when fixed_length is %s"),
            max_length_max, fixed_length ? "True" : "False");
    }
    self->max_length = max_length;
    self->fixed_length = fixed_length;

    // Room for max_length bytes, written in place (Adapter.h); a CCCD the phone writes between two
    // VMs allocates nothing.
    self->value = bleio_attribute_value_new(self, max_length, NULL);
    common_hal_bleio_descriptor_set_value(self, initial_value_bufinfo);
}

bleio_uuid_obj_t *common_hal_bleio_descriptor_get_uuid(bleio_descriptor_obj_t *self) {
    return self->uuid;
}

bleio_characteristic_obj_t *common_hal_bleio_descriptor_get_characteristic(bleio_descriptor_obj_t *self) {
    return self->characteristic;
}

size_t common_hal_bleio_descriptor_get_value(bleio_descriptor_obj_t *self, uint8_t *buf, size_t len) {
    // Do GATT operations only if this descriptor has been registered
    if (self->handle != BLE_GATT_HANDLE_INVALID) {
        if (common_hal_bleio_service_get_is_remote(self->characteristic->service)) {
            const uint16_t conn_handle = bleio_connection_get_conn_handle(self->characteristic->service->connection);
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

void common_hal_bleio_descriptor_set_value(bleio_descriptor_obj_t *self, mp_buffer_info_t *bufinfo) {
    if (self->fixed_length && bufinfo->len != self->max_length) {
        mp_raise_ValueError(MP_ERROR_TEXT("Value length != required fixed length"));
    }
    if (bufinfo->len > self->max_length) {
        mp_raise_ValueError(MP_ERROR_TEXT("Value length > max_length"));
    }

    bleio_descriptor_set_local_value(self, bufinfo);

    // Do GATT operations only if this descriptor has been registered.
    if (self->handle != BLE_GATT_HANDLE_INVALID) {
        if (common_hal_bleio_service_get_is_remote(self->characteristic->service)) {
            const uint16_t conn_handle = bleio_connection_get_conn_handle(self->characteristic->service->connection);
            bleio_check_connected(conn_handle);
            att_write_value(conn_handle, self->handle, bufinfo->buf, bufinfo->len, true);
        } else {
            // Written locally above, even if no connections are active.
        }
    }
}

bool bleio_descriptor_set_local_value(bleio_descriptor_obj_t *self, mp_buffer_info_t *bufinfo) {
    if ((self->fixed_length && bufinfo->len != self->max_length) || bufinfo->len > self->max_length) {
        return false;
    }
    bleio_attribute_value_set(self->value, bufinfo->buf, bufinfo->len);
    return true;
}
