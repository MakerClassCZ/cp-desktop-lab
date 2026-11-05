// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019-2020 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <string.h>
#include <stdio.h>

#include "shared/runtime/interrupt_char.h"
#include "py/runtime.h"
#include "py/stream.h"

#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Adapter.h"
#include "shared-bindings/_bleio/Characteristic.h"
#include "shared-bindings/_bleio/Connection.h"
#include "shared-bindings/_bleio/PacketBuffer.h"
#include "supervisor/shared/tick.h"

static void write_to_ringbuf(bleio_packet_buffer_obj_t *self, uint8_t *data, uint16_t len) {
    if (len + sizeof(uint16_t) > ringbuf_size(&self->ringbuf)) {
        // This shouldn't happen.
        return;
    }
    // Push all the data onto the ring buffer.
    // Make room for the new value by dropping the oldest packets first.
    while (ringbuf_size(&self->ringbuf) - ringbuf_num_filled(&self->ringbuf) < len + sizeof(uint16_t)) {
        uint16_t packet_length;
        ringbuf_get_n(&self->ringbuf, (uint8_t *)&packet_length, sizeof(uint16_t));
        for (uint16_t i = 0; i < packet_length; i++) {
            ringbuf_get(&self->ringbuf);
        }
        // set an overflow flag?
    }
    ringbuf_put_n(&self->ringbuf, (uint8_t *)&len, sizeof(uint16_t));
    ringbuf_put_n(&self->ringbuf, data, len);
}

// Whether any packet buffer holds data not yet sent: the background then sends it.
static bool _pending;

// The pending outgoing buffer sent now, as one notification (or a write, as a client), and the
// other one made pending. An HCI send hands the bytes to the controller before it returns, so a
// buffer is free again at once; what pending gathers between two sends goes out in one packet.
static void send_pending(bleio_packet_buffer_obj_t *self) {
    if (self->pending_size == 0) {
        return;
    }
    mp_buffer_info_t bufinfo = {
        .buf = self->outgoing[self->pending_index],
        .len = self->pending_size,
    };
    self->pending_size = 0;
    self->pending_index = (self->pending_index + 1) % 2;
    if (self->conn_handle != BLE_CONN_HANDLE_INVALID) {
        common_hal_bleio_characteristic_set_value(self->characteristic, &bufinfo);
    }
}

void bleio_packet_buffer_update(bleio_packet_buffer_obj_t *self, mp_buffer_info_t *bufinfo) {
    write_to_ringbuf(self, bufinfo->buf, bufinfo->len);
}

void bleio_packet_buffer_peer(bleio_packet_buffer_obj_t *self, uint16_t conn_handle) {
    if (!self->client) {
        self->conn_handle = conn_handle;
    }
}

void bleio_packet_buffer_disconnected(bleio_packet_buffer_obj_t *self, uint16_t conn_handle) {
    if (self->conn_handle == conn_handle) {
        self->conn_handle = BLE_CONN_HANDLE_INVALID;
        self->pending_size = 0;
    }
}

void bleio_packet_buffer_background(void) {
    if (!_pending) {
        return;
    }
    _pending = false;
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    if (!adapter->enabled) {
        return;
    }
    const uint16_t max_handle = bleio_adapter_max_attribute_handle(adapter);
    for (uint16_t handle = 1; handle <= max_handle; handle++) {
        mp_obj_t attribute = bleio_adapter_get_attribute(adapter, handle);
        if (!mp_obj_is_type(attribute, &bleio_characteristic_type)) {
            continue;
        }
        bleio_characteristic_obj_t *characteristic = MP_OBJ_TO_PTR(attribute);
        if (handle == characteristic->handle &&
            mp_obj_is_type(characteristic->observer, &bleio_packet_buffer_type)) {
            send_pending(MP_OBJ_TO_PTR(characteristic->observer));
        }
    }
}

// Buffers the caller gives (the BLE workflow's, static), so nothing is allocated.
void _common_hal_bleio_packet_buffer_construct(
    bleio_packet_buffer_obj_t *self, bleio_characteristic_obj_t *characteristic,
    uint32_t *incoming_buffer, size_t incoming_buffer_size,
    uint32_t *outgoing_buffer1, uint32_t *outgoing_buffer2, size_t max_packet_size,
    ble_event_handler_t *static_handler_entry) {
    (void)static_handler_entry;

    self->characteristic = characteristic;
    self->client = self->characteristic->service->is_remote;
    bleio_characteristic_properties_t incoming =
        self->characteristic->props & (CHAR_PROP_WRITE_NO_RESPONSE | CHAR_PROP_WRITE);
    bleio_characteristic_properties_t outgoing =
        self->characteristic->props & (CHAR_PROP_NOTIFY | CHAR_PROP_INDICATE);

    if (self->client) {
        // Swap if we're the client.
        bleio_characteristic_properties_t temp = incoming;
        incoming = outgoing;
        outgoing = temp;
        self->conn_handle = bleio_connection_get_conn_handle(MP_OBJ_TO_PTR(self->characteristic->service->connection));
    } else {
        // The peer that writes the characteristic or its CCCD (bleio_packet_buffer_peer()).
        self->conn_handle = BLE_CONN_HANDLE_INVALID;
    }

    if (incoming && incoming_buffer != NULL) {
        ringbuf_init(&self->ringbuf, (uint8_t *)incoming_buffer, incoming_buffer_size);
    } else {
        ringbuf_init(&self->ringbuf, NULL, 0);
    }

    self->packet_queued = false;
    self->pending_index = 0;
    self->pending_size = 0;
    if (outgoing) {
        self->outgoing[0] = (uint8_t *)outgoing_buffer1;
        self->outgoing[1] = (uint8_t *)outgoing_buffer2;
    } else {
        self->outgoing[0] = NULL;
        self->outgoing[1] = NULL;
    }
    self->max_packet_size = max_packet_size;

    bleio_characteristic_set_observer(self->characteristic, self);
}

void common_hal_bleio_packet_buffer_construct(
    bleio_packet_buffer_obj_t *self, bleio_characteristic_obj_t *characteristic,
    size_t buffer_size, size_t max_packet_size) {
    const size_t incoming_size = buffer_size * (sizeof(uint16_t) + max_packet_size);
    uint32_t *incoming = m_malloc_maybe((incoming_size + 3) / 4 * 4);
    if (incoming == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("Buffer too large and unable to allocate"));
    }
    uint32_t *outgoing1 = m_malloc((max_packet_size + 3) / 4 * 4);
    uint32_t *outgoing2 = m_malloc((max_packet_size + 3) / 4 * 4);
    _common_hal_bleio_packet_buffer_construct(self, characteristic, incoming, incoming_size,
        outgoing1, outgoing2, max_packet_size, NULL);
}

mp_int_t common_hal_bleio_packet_buffer_readinto(bleio_packet_buffer_obj_t *self, uint8_t *data, size_t len) {
    if (ringbuf_num_filled(&self->ringbuf) < 2) {
        return 0;
    }

    // Copy received data.
    // Get packet length, which is in first two bytes of packet.
    uint16_t packet_length;
    ringbuf_get_n(&self->ringbuf, (uint8_t *)&packet_length, sizeof(uint16_t));

    mp_int_t ret;
    if (packet_length > len) {
        // Packet is longer than requested. Return negative of overrun value.
        ret = len - packet_length;
        // Discard the packet if it's too large. Don't fill data.
        while (packet_length--) {
            (void)ringbuf_get(&self->ringbuf);
        }
    } else {
        // Read as much as possible, but might be shorter than len.
        ringbuf_get_n(&self->ringbuf, data, packet_length);
        ret = packet_length;
    }

    return ret;
}

mp_int_t common_hal_bleio_packet_buffer_write(bleio_packet_buffer_obj_t *self,
    const uint8_t *data, size_t len, uint8_t *header, size_t header_len) {
    if (self->outgoing[0] == NULL) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Writes not supported on Characteristic"));
    }
    if (self->conn_handle == BLE_CONN_HANDLE_INVALID) {
        return -1;
    }
    uint16_t outgoing_packet_length = common_hal_bleio_packet_buffer_get_outgoing_packet_length(self);

    if (len + header_len > outgoing_packet_length) {
        // Supplied data will not fit in a single BLE packet.
        mp_raise_ValueError(MP_ERROR_TEXT("Total data to write is larger than outgoing_packet_length"));
    }

    if (len + self->pending_size > outgoing_packet_length) {
        // No room to append len bytes to the packet: what it holds goes out first.
        send_pending(self);
    }
    if (self->conn_handle == BLE_CONN_HANDLE_INVALID) {
        return -1;
    }

    size_t num_bytes_written = 0;

    uint8_t *pending = self->outgoing[self->pending_index];

    if (self->pending_size == 0) {
        memcpy(pending, header, header_len);
        self->pending_size += header_len;
        num_bytes_written += header_len;
    }
    memcpy(pending + self->pending_size, data, len);
    self->pending_size += len;
    num_bytes_written += len;

    // Sent when the next write does not fit, at a flush, or by the background soon after.
    _pending = true;
    return num_bytes_written;
}

void common_hal_bleio_packet_buffer_flush(bleio_packet_buffer_obj_t *self) {
    if (!common_hal_bleio_packet_buffer_deinited(self) && self->outgoing[0] != NULL) {
        send_pending(self);
    }
}

mp_int_t common_hal_bleio_packet_buffer_get_incoming_packet_length(bleio_packet_buffer_obj_t *self) {
    // If this PacketBuffer is coming from a remote service via NOTIFY or INDICATE
    // the maximum size is what can be sent in one
    // BLE packet. But we must be connected to know that value.
    //
    // Otherwise it can be as long as the characteristic
    // will permit, whether or not we're connected.

    if (self->characteristic == NULL) {
        return -1;
    }

    if (self->characteristic->service != NULL &&
        self->characteristic->service->is_remote &&
        (common_hal_bleio_characteristic_get_properties(self->characteristic) &
         (CHAR_PROP_INDICATE | CHAR_PROP_NOTIFY))) {
        // We are talking to a remote service, and data is arriving via NOTIFY or INDICATE.
        if (self->conn_handle != BLE_CONN_HANDLE_INVALID) {
            bleio_connection_internal_t *connection = bleio_conn_handle_to_connection(self->conn_handle);
            if (connection) {
                return MIN(common_hal_bleio_connection_get_max_packet_length(connection),
                    self->characteristic->max_length);
            }
        }
        // There's no current connection, so we don't know the MTU, and
        // we can't tell what the largest incoming packet length would be.
        return -1;
    }
    return self->characteristic->max_length;
}

mp_int_t common_hal_bleio_packet_buffer_get_outgoing_packet_length(bleio_packet_buffer_obj_t *self) {
    // If we are sending data via NOTIFY or INDICATE, the maximum size
    // is what can be sent in one BLE packet. But we must be connected
    // to know that value.
    //
    // Otherwise it can be as long as the characteristic
    // will permit, whether or not we're connected.

    if (self->characteristic == NULL) {
        return -1;
    }

    if (self->characteristic->service != NULL &&
        !self->characteristic->service->is_remote &&
        (common_hal_bleio_characteristic_get_properties(self->characteristic) &
         (CHAR_PROP_INDICATE | CHAR_PROP_NOTIFY))) {
        // We are sending to a client, via NOTIFY or INDICATE.
        if (self->conn_handle != BLE_CONN_HANDLE_INVALID) {
            bleio_connection_internal_t *connection = bleio_conn_handle_to_connection(self->conn_handle);
            if (connection) {
                return MIN(MIN(common_hal_bleio_connection_get_max_packet_length(connection),
                    self->max_packet_size),
                    self->characteristic->max_length);
            }
        }
        // There's no current connection, so we don't know the MTU, and
        // we can't tell what the largest outgoing packet length would be.
        return -1;
    }
    return MIN(self->characteristic->max_length, self->max_packet_size);
}

bool common_hal_bleio_packet_buffer_deinited(bleio_packet_buffer_obj_t *self) {
    return self->characteristic == NULL;
}

void common_hal_bleio_packet_buffer_deinit(bleio_packet_buffer_obj_t *self) {
    if (!common_hal_bleio_packet_buffer_deinited(self)) {
        bleio_characteristic_clear_observer(self->characteristic);
        ringbuf_deinit(&self->ringbuf);
        self->pending_size = 0;
        // Mark as deinited, so common_hal_bleio_packet_buffer_deinited() reports it.
        self->characteristic = NULL;
    }
}

bool common_hal_bleio_packet_buffer_connected(bleio_packet_buffer_obj_t *self) {
    return !common_hal_bleio_packet_buffer_deinited(self) && self->conn_handle != BLE_CONN_HANDLE_INVALID;
}
