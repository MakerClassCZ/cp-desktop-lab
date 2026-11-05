// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// What the port sees of the HCI between _bleio and its controller, the same for the virtual one
// and a real one: the state (advertising, the name, the connection) for the API and the panel,
// a line in the bus trace for what matters (BLE ADV, CONNECT, ATT, DISCONNECT), and every packet
// in a btsnoop file with --ble-log, which Wireshark opens.

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ble_host.h"
#include "bus_trace.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#include "common-hal/_bleio/Adapter.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Characteristic.h"

static pthread_mutex_t _lock = PTHREAD_MUTEX_INITIALIZER;
static ble_host_state_t _state;
static uint8_t _public_address[6];
static uint8_t _random_address[6];
static uint8_t _own_address_type;
static uint8_t _adv_type;
static FILE *_log;

bool ble_host_open_log(const char *path) {
    _log = fopen(path, "wb");
    if (_log == NULL) {
        perror(path);
        return false;
    }
    // btsnoop: version 1, datalink 1002 (HCI with its H4 type byte first).
    static const uint8_t header[16] = { 'b', 't', 's', 'n', 'o', 'o', 'p', 0, 0, 0, 0, 1, 0, 0, 0x03, 0xea };
    fwrite(header, 1, sizeof(header), _log);
    fflush(_log);
    return true;
}

static void _be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void _log_packet(bool to_controller, const uint8_t *h4, size_t len) {
    uint8_t record[24];
    _be32(record, (uint32_t)len);
    _be32(record + 4, (uint32_t)len);
    // Bit 0: received (controller to host); bit 1: a command or an event, not data.
    _be32(record + 8, (to_controller ? 0 : 1) | (h4[0] == 0x01 || h4[0] == 0x04 ? 2 : 0));
    _be32(record + 12, 0);
    // Microseconds since year 0, as btsnoop counts them.
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t us = 0x00dcddb30f2f8000ULL + (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
    _be32(record + 16, (uint32_t)(us >> 32));
    _be32(record + 20, (uint32_t)us);
    fwrite(record, 1, sizeof(record), _log);
    fwrite(h4, 1, len, _log);
    fflush(_log);
}

// The name an advertisement carries: its complete or shortened local name, else "".
static void _name(const uint8_t *data, size_t len, char *name, size_t cap) {
    name[0] = '\0';
    for (size_t i = 0; i + 1 < len && data[i] != 0 && i + data[i] < len; i += data[i] + 1) {
        if (data[i + 1] == 0x09 || data[i + 1] == 0x08) {
            size_t n = (size_t)data[i] - 1;
            n = n < cap - 1 ? n : cap - 1;
            memcpy(name, data + i + 2, n);
            name[n] = '\0';
            if (data[i + 1] == 0x09) {
                return;
            }
        }
    }
}

static void _hex(char *out, size_t cap, const uint8_t *data, size_t len) {
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; i < len && n + 4 < cap; i++) {
        n += (size_t)snprintf(out + n, cap - n, i ? " %02x" : "%02x", data[i]);
    }
}

static const char *_att_name(uint8_t op) {
    switch (op) {
        case 0x01:
            return "ERROR_RSP";
        case 0x02:
            return "MTU_REQ";
        case 0x03:
            return "MTU_RSP";
        case 0x04:
            return "FIND_INFO_REQ";
        case 0x05:
            return "FIND_INFO_RSP";
        case 0x06:
            return "FIND_TYPE_REQ";
        case 0x07:
            return "FIND_TYPE_RSP";
        case 0x08:
            return "READ_TYPE_REQ";
        case 0x09:
            return "READ_TYPE_RSP";
        case 0x0a:
            return "READ_REQ";
        case 0x0b:
            return "READ_RSP";
        case 0x0c:
            return "READ_BLOB_REQ";
        case 0x0d:
            return "READ_BLOB_RSP";
        case 0x10:
            return "READ_GROUP_REQ";
        case 0x11:
            return "READ_GROUP_RSP";
        case 0x12:
            return "WRITE_REQ";
        case 0x13:
            return "WRITE_RSP";
        case 0x1b:
            return "NOTIFY";
        case 0x1d:
            return "INDICATE";
        case 0x1e:
            return "CONFIRM";
        case 0x52:
            return "WRITE_CMD";
        default:
            return NULL;
    }
}

static uint16_t _ft_handle(void);
static void _ft_bytes(bool to_phone, const uint8_t *data, size_t len);

// An ATT PDU as a trace line: its name, the handle where it has one, the rest in hex and, when
// it is text, as text too.
static void _trace_att(bool to_phone, const uint8_t *pdu, size_t len) {
    if (len == 0 || !bus_trace_on()) {
        return;
    }
    const char *name = _att_name(pdu[0]);
    char what[24];
    if (name == NULL) {
        snprintf(what, sizeof(what), "0x%02x", pdu[0]);
        name = what;
    }
    bool handled = pdu[0] == 0x0a || pdu[0] == 0x0c || pdu[0] == 0x12 || pdu[0] == 0x1b || pdu[0] == 0x1d
        || pdu[0] == 0x52;
    size_t skip = handled && len >= 3 ? 3 : 1;
    char hex[3 * 64 + 8];
    _hex(hex, sizeof(hex), pdu + skip, len - skip > 64 ? 64 : len - skip);
    char text[72] = "";
    bool printable = len > skip;
    for (size_t i = skip; i < len && i - skip < 64; i++) {
        printable = printable && pdu[i] >= 0x20 && pdu[i] < 0x7f && pdu[i] != '"';
    }
    if (printable) {
        size_t n = len - skip < 64 ? len - skip : 64;
        text[0] = ' ';
        text[1] = '"';
        memcpy(text + 2, pdu + skip, n);
        text[2 + n] = '"';
        text[3 + n] = '\0';
    }
    // The file transfer's PDUs are traced as its commands and answers.
    const uint16_t handle = handled && len >= 3 ? (uint16_t)(pdu[1] | pdu[2] << 8) : 0;
    if ((pdu[0] == 0x52 || pdu[0] == 0x1b) && handle != 0 && handle == _ft_handle()) {
        _ft_bytes(to_phone, pdu + 3, len - 3);
        return;
    }
    if (handled && len >= 3) {
        bus_trace_text("BLE ATT %s %s 0x%04x %s%s", to_phone ? "->" : "<-", name, handle, hex, text);
    } else {
        bus_trace_text("BLE ATT %s %s %s%s", to_phone ? "->" : "<-", name, hex, text);
    }
}

// ---- the BLE workflow's file transfer, decoded (supervisor/shared/bluetooth/file_transfer_protocol.h) ----

// The transfer characteristic's value handle: adaf0200-4669-6c65-5472-616e73666572, in the
// attribute table's static part (the supervisor's); 0 when it is not there.
static uint16_t _ft_handle(void) {
    static const uint8_t uuid[16] = { 0x72, 0x65, 0x66, 0x73, 0x6e, 0x61, 0x72, 0x54, 0x65, 0x6c, 0x69, 0x46, 0x00, 0x02, 0xaf, 0xad };
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    if (!adapter->enabled) {
        return 0;
    }
    for (uint16_t i = 1; i < adapter->static_attribute_count; i++) {
        mp_obj_t a = adapter->static_attributes[i];
        if (mp_obj_is_type(a, &bleio_characteristic_type)) {
            bleio_characteristic_obj_t *c = MP_OBJ_TO_PTR(a);
            if (c->handle == i && c->uuid->size == 128 && memcmp(c->uuid->uuid128, uuid, 16) == 0) {
                return i;
            }
        }
    }
    return 0;
}

static const char *_ft_status(uint8_t status) {
    switch (status) {
        case 0x01:
            return "ok";
        case 0x02:
            return "error";
        case 0x03:
            return "error_no_file";
        case 0x04:
            return "error_protocol";
        case 0x05:
            return "error_readonly";
        default:
            return "?";
    }
}

static uint32_t _u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t _u16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

// One direction's stream: the bytes of a command or an answer gathered until it is whole.
typedef struct {
    uint8_t buf[1100];
    size_t have;
    size_t skip;                // payload bytes still to pass over (a long path, data)
} ft_stream_t;
static ft_stream_t _ft_in, _ft_out;
static uint32_t _ft_write_total;

// A path of n bytes at p, as text.
static void _ft_path(char *out, size_t cap, const uint8_t *p, size_t n) {
    n = n < cap - 1 ? n : cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

// How long the command or answer at the stream's start is: its header and what follows it; 0
// while the header is not all there, SIZE_MAX for an opcode the protocol does not have.
static size_t _ft_length(const uint8_t *b, size_t have) {
    #define NEED(n) if (have < (n)) return 0
    switch (b[0]) {
        case 0x10:              // READ: path
            NEED(12);
            return 12 + _u16(b + 2);
        case 0x12:              // READ_PACING
            return 12;
        case 0x20:              // WRITE: path
            NEED(20);
            return 20 + _u16(b + 2);
        case 0x22:              // WRITE_DATA: data
            NEED(12);
            return 12 + _u32(b + 8);
        case 0x30:              // DELETE, LISTDIR: path
        case 0x50:
            NEED(4);
            return 4 + _u16(b + 2);
        case 0x40:              // MKDIR: path
            NEED(16);
            return 16 + _u16(b + 2);
        case 0x60:              // MOVE: two paths and a byte between them
            NEED(6);
            return 6 + _u16(b + 2) + 1 + _u16(b + 4);
        case 0x11:              // READ_DATA: data
            NEED(16);
            return 16 + _u32(b + 12);
        case 0x21:              // WRITE_PACING
            return 20;
        case 0x31:              // DELETE_STATUS, MOVE_STATUS
        case 0x61:
            return 2;
        case 0x41:              // MKDIR_STATUS
            return 16;
        case 0x51:              // LISTDIR_ENTRY: name
            NEED(28);
            return 28 + _u16(b + 2);
        default:
            return SIZE_MAX;
    }
#undef NEED
}

// A whole command (from the phone) or answer (to it) as one trace line; data only counted.
static void _ft_line(bool to_phone, const uint8_t *b, size_t len) {
    char path[96], other[96];
    const char *dir = to_phone ? "->" : "<-";
    switch (b[0]) {
        case 0x10:
            _ft_path(path, sizeof(path), b + 12, len - 12);
            bus_trace_text("BLE FT %s READ %s at %u, %u bytes", dir, path, (unsigned)_u32(b + 4), (unsigned)_u32(b + 8));
            break;
        case 0x12:
            bus_trace_text("BLE FT %s READ_PACING at %u, %u bytes", dir, (unsigned)_u32(b + 4), (unsigned)_u32(b + 8));
            break;
        case 0x20:
            _ft_write_total = _u32(b + 16);
            _ft_path(path, sizeof(path), b + 20, len - 20);
            bus_trace_text("BLE FT %s WRITE %s %u bytes", dir, path, (unsigned)_ft_write_total);
            break;
        case 0x22:
            bus_trace_text("BLE FT %s WRITE_DATA at %u, %u bytes", dir, (unsigned)_u32(b + 4), (unsigned)_u32(b + 8));
            break;
        case 0x30:
            _ft_path(path, sizeof(path), b + 4, len - 4);
            bus_trace_text("BLE FT %s DELETE %s", dir, path);
            break;
        case 0x40:
            _ft_path(path, sizeof(path), b + 16, len - 16);
            bus_trace_text("BLE FT %s MKDIR %s", dir, path);
            break;
        case 0x50:
            _ft_path(path, sizeof(path), b + 4, len - 4);
            bus_trace_text("BLE FT %s LISTDIR %s", dir, path);
            break;
        case 0x60: {
            // Both paths within what the buffer holds (a long first one cuts the second).
            const size_t old_len = MIN((size_t)_u16(b + 2), len - 6);
            const size_t new_at = MIN(6 + old_len + 1, len);
            _ft_path(path, sizeof(path), b + 6, old_len);
            _ft_path(other, sizeof(other), b + new_at, MIN((size_t)_u16(b + 4), len - new_at));
            bus_trace_text("BLE FT %s MOVE %s %s", dir, path, other);
            break;
        }
        case 0x11:
            bus_trace_text("BLE FT %s READ_DATA %s %u/%u, %u bytes", dir, _ft_status(b[1]), (unsigned)_u32(b + 4),
                (unsigned)_u32(b + 8), (unsigned)_u32(b + 12));
            break;
        case 0x21:
            bus_trace_text("BLE FT %s WRITE_PACING %s %u/%u", dir, _ft_status(b[1]), (unsigned)_u32(b + 4),
                (unsigned)_ft_write_total);
            break;
        case 0x31:
            bus_trace_text("BLE FT %s DELETE_STATUS %s", dir, _ft_status(b[1]));
            break;
        case 0x41:
            bus_trace_text("BLE FT %s MKDIR_STATUS %s", dir, _ft_status(b[1]));
            break;
        case 0x61:
            bus_trace_text("BLE FT %s MOVE_STATUS %s", dir, _ft_status(b[1]));
            break;
        case 0x51:
            if (b[1] != 0x01 || _u32(b + 4) >= _u32(b + 8)) {
                bus_trace_text("BLE FT %s LISTDIR_ENTRY %s %u/%u", dir, _ft_status(b[1]), (unsigned)_u32(b + 4),
                    (unsigned)_u32(b + 8));
            } else {
                _ft_path(path, sizeof(path), b + 28, len - 28);
                bus_trace_text("BLE FT %s LISTDIR_ENTRY %u/%u %s%s %u", dir, (unsigned)_u32(b + 4), (unsigned)_u32(b + 8),
                    path, _u32(b + 12) & 1 ? "/" : "", (unsigned)_u32(b + 24));
            }
            break;
    }
}

// What a PDU carries on the transfer characteristic, into its direction's stream.
static void _ft_bytes(bool to_phone, const uint8_t *data, size_t len) {
    ft_stream_t *s = to_phone ? &_ft_out : &_ft_in;
    while (len > 0) {
        if (s->skip > 0) {
            size_t n = len < s->skip ? len : s->skip;
            s->skip -= n;
            data += n;
            len -= n;
            continue;
        }
        size_t n = sizeof(s->buf) - s->have < len ? sizeof(s->buf) - s->have : len;
        memcpy(s->buf + s->have, data, n);
        s->have += n;
        data += n;
        len -= n;
        while (s->have > 0) {
            size_t whole = _ft_length(s->buf, s->have);
            if (whole == SIZE_MAX) {
                s->have = 0;    // not the protocol (a long path's rest): dropped
            } else if (whole == 0 || (whole > s->have && whole <= sizeof(s->buf))) {
                break;
            } else if (whole > sizeof(s->buf)) {
                // Longer than the buffer: the header's line now, the rest passed over.
                _ft_line(to_phone, s->buf, s->have);
                s->skip = whole - s->have;
                s->have = 0;
            } else {
                _ft_line(to_phone, s->buf, whole);
                memmove(s->buf, s->buf + whole, s->have - whole);
                s->have -= whole;
            }
        }
    }
}

static void _command(const uint8_t *h4, size_t len) {
    uint16_t opcode = (uint16_t)(h4[1] | h4[2] << 8);
    const uint8_t *p = h4 + 4;
    size_t n = len - 4;
    switch (opcode) {
        case 0x0c03:            // Reset
            _state.advertising = _state.connected = false;
            _state.link_count = 0;
            break;
        case 0x2005:            // LE Set Random Address
            if (n >= 6) {
                memcpy(_random_address, p, 6);
            }
            break;
        case 0x2006:            // LE Set Advertising Parameters
            if (n >= 6) {
                _adv_type = p[4];
                _own_address_type = p[5];
            }
            break;
        case 0x2008:            // LE Set Advertising Data
        case 0x2009:            // LE Set Scan Response Data
            if (n >= 1 && p[0] <= 31 && p[0] < n) {
                memcpy(opcode == 0x2008 ? _state.data : _state.scan_response, p + 1, p[0]);
                *(opcode == 0x2008 ? &_state.data_len : &_state.scan_response_len) = p[0];
            }
            break;
        case 0x200a: {          // LE Set Advertising Enable
            _state.advertising = n >= 1 && p[0] != 0;
            _state.connectable = _adv_type == 0x00 || _adv_type == 0x01;
            _state.public = _own_address_type == 0x00;
            memcpy(_state.address, _state.public ? _public_address : _random_address, 6);
            _name(_state.data, _state.data_len, _state.name, sizeof(_state.name));
            if (_state.name[0] == '\0') {
                _name(_state.scan_response, _state.scan_response_len, _state.name, sizeof(_state.name));
            }
            char hex[31 * 3 + 1];
            _hex(hex, sizeof(hex), _state.data, _state.data_len);
            if (_state.advertising) {
                bus_trace_text("BLE ADV \"%s\" %s %s", _state.name,
                    _state.connectable ? "connectable" : _adv_type == 0x02 ? "scannable" : "nonconnectable", hex);
            } else {
                bus_trace_text("BLE ADV off");
            }
            break;
        }
    }
}

static ble_host_link_t *_link(uint16_t handle) {
    for (uint8_t i = 0; i < _state.link_count; i++) {
        if (_state.links[i].handle == handle) {
            return &_state.links[i];
        }
    }
    return NULL;
}

static void _event(const uint8_t *h4, size_t len) {
    const uint8_t *p = h4 + 3;
    size_t n = len - 3;
    if (h4[1] == 0x0e && n >= 10 && (p[1] | p[2] << 8) == 0x1009 && p[3] == 0) {
        // Command Complete of Read BD_ADDR: the public address.
        memcpy(_public_address, p + 4, 6);
    } else if (h4[1] == 0x3e && n >= 12 && p[0] == 0x01 && p[1] == 0) {
        // LE Connection Complete: the advertising stops, the peer is connected.
        _state.connected = true;
        _state.advertising = false;
        memcpy(_state.peer, p + 6, 6);
        if (_state.link_count < BLE_HOST_LINKS) {
            ble_host_link_t *link = &_state.links[_state.link_count++];
            link->handle = (uint16_t)(p[2] | p[3] << 8) & 0x0fff;
            link->central = p[4] == 0x00;
            link->encrypted = false;
            memcpy(link->peer, p + 6, 6);
        }
        bus_trace_text("BLE CONNECT %02x:%02x:%02x:%02x:%02x:%02x", p[11], p[10], p[9], p[8], p[7], p[6]);
    } else if (h4[1] == 0x05 && n >= 4 && p[0] == 0) {
        ble_host_link_t *link = _link((uint16_t)(p[1] | p[2] << 8) & 0x0fff);
        if (link != NULL) {
            ble_host_link_t *end = &_state.links[_state.link_count--];
            memmove(link, link + 1, (size_t)(end - (link + 1)) * sizeof(*link));
        }
        _state.connected = _state.link_count > 0;
        _ft_in.have = _ft_out.have = _ft_in.skip = _ft_out.skip = 0;
        bus_trace_text("BLE DISCONNECT 0x%02x", p[3]);
    } else if ((h4[1] == 0x08 && n >= 4) || (h4[1] == 0x30 && n >= 3)) {
        // Encryption Change (its on or off), Encryption Key Refresh Complete (on).
        ble_host_link_t *link = _link((uint16_t)(p[1] | p[2] << 8) & 0x0fff);
        if (link != NULL && p[0] == 0) {
            link->encrypted = h4[1] == 0x30 || p[3] != 0;
        }
    }
}

void ble_host_packet(bool to_controller, const uint8_t *h4, size_t len) {
    pthread_mutex_lock(&_lock);
    if (_log != NULL) {
        _log_packet(to_controller, h4, len);
    }
    if (h4[0] == 0x01 && len >= 4) {
        _command(h4, len);
    } else if (h4[0] == 0x04 && len >= 3) {
        _event(h4, len);
    } else if (h4[0] == 0x02 && len >= 9 && (h4[2] >> 4 & 0x3) != 0x1 && (h4[7] | h4[8] << 8) == 0x0004) {
        // ACL, the first fragment of an L2CAP frame on the ATT channel.
        _trace_att(to_controller, h4 + 9, len - 9);
    }
    pthread_mutex_unlock(&_lock);
}

void ble_host_state(ble_host_state_t *state) {
    pthread_mutex_lock(&_lock);
    *state = _state;
    pthread_mutex_unlock(&_lock);
}
