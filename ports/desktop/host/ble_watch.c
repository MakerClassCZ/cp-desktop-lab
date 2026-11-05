// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// What the port sees of the HCI between _bleio and its controller, the same for the virtual one
// and a real one: the state (advertising, scanning, the name, the connections, their encryption,
// the bonds) for the API and the panel, a line in the bus trace for what matters (BLE ADV, SCAN,
// REPORT, CONNECT, ATT, PAIR, LTK, ENCRYPTED, BOND, DISCONNECT), and every packet in a btsnoop
// file with --ble-log, which Wireshark opens. The trace never shows a key; the btsnoop file has
// them all, as a real HCI log does.

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ble_host.h"
#include "bus_trace.h"

#include "common-hal/_bleio/Adapter.h"
#include "common-hal/_bleio/Connection.h"
#include "common-hal/_bleio/bonding.h"
#include "shared-bindings/_bleio/__init__.h"

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

static bool _scan_active;

// What a scan reported, so the trace has each address and data once per scan.
#define SEEN_COUNT 64
static struct {
    uint8_t address[6];
    uint32_t hash;
} _seen[SEEN_COUNT];
static size_t _seen_count;

// The names scans found, by address: a central's connection shows its peer by name.
#define NAMES_COUNT 16
static struct {
    uint8_t address[6];
    char name[32];
} _names[NAMES_COUNT];
static size_t _names_next;

static const char *_name_of(const uint8_t address[6]) {
    for (size_t i = 0; i < NAMES_COUNT; i++) {
        if (_names[i].name[0] != '\0' && memcmp(_names[i].address, address, 6) == 0) {
            return _names[i].name;
        }
    }
    return NULL;
}

static void _remember_name(const uint8_t address[6], const char *name) {
    if (name[0] == '\0') {
        return;
    }
    for (size_t i = 0; i < NAMES_COUNT; i++) {
        if (_names[i].name[0] != '\0' && memcmp(_names[i].address, address, 6) == 0) {
            snprintf(_names[i].name, sizeof(_names[i].name), "%s", name);
            return;
        }
    }
    memcpy(_names[_names_next].address, address, 6);
    snprintf(_names[_names_next].name, sizeof(_names[_names_next].name), "%s", name);
    _names_next = (_names_next + 1) % NAMES_COUNT;
}

static void _scan(bool on) {
    if (on == _state.scanning) {
        return;
    }
    _state.scanning = on;
    _seen_count = 0;
    if (on) {
        bus_trace_text("BLE SCAN on %s", _scan_active ? "active" : "passive");
    } else {
        bus_trace_text("BLE SCAN off");
    }
}

static const char *_report_kind(uint8_t type) {
    static const char *const kinds[] = { "ADV_IND", "ADV_DIRECT_IND", "ADV_SCAN_IND", "ADV_NONCONN_IND", "SCAN_RSP" };
    return type < 5 ? kinds[type] : "ADV";
}

// One advertising report: its line in the trace, once per address and data in a scan.
static void _report(const char *kind, const uint8_t address[6], int8_t rssi, const uint8_t *data, size_t len) {
    char name[32];
    _name(data, len, name, sizeof(name));
    _remember_name(address, name);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash = (hash ^ data[i]) * 16777619u;
    }
    hash ^= (uint32_t)kind[0] << 24 | (uint32_t)strlen(kind);
    for (size_t i = 0; i < _seen_count; i++) {
        if (_seen[i].hash == hash && memcmp(_seen[i].address, address, 6) == 0) {
            return;
        }
    }
    if (_seen_count < SEEN_COUNT) {
        memcpy(_seen[_seen_count].address, address, 6);
        _seen[_seen_count++].hash = hash;
    }
    char hex[31 * 3 + 1];
    _hex(hex, sizeof(hex), data, len > 31 ? 31 : len);
    bus_trace_text("BLE REPORT %02x:%02x:%02x:%02x:%02x:%02x %s %d \"%s\" %s", address[5], address[4], address[3],
        address[2], address[1], address[0], kind, rssi, name, hex);
}

// LE Advertising Report: the number of reports, each its type, address, data and RSSI.
static void _reports(const uint8_t *p, size_t n) {
    size_t at = 2;
    for (uint8_t i = 0; i < p[1] && at + 9 <= n; i++) {
        const uint8_t *r = p + at;
        size_t len = r[8];
        if (at + 10 + len > n) {
            return;
        }
        _report(_report_kind(r[0]), r + 2, (int8_t)r[9 + len], r + 9, len);
        at += 10 + len;
    }
}

// LE Extended Advertising Report: each its event type bits, address, PHYs, SID, power, RSSI,
// interval, direct address, data.
static void _extended_reports(const uint8_t *p, size_t n) {
    size_t at = 2;
    for (uint8_t i = 0; i < p[1] && at + 24 <= n; i++) {
        const uint8_t *r = p + at;
        size_t len = r[23];
        if (at + 24 + len > n) {
            return;
        }
        uint16_t type = (uint16_t)(r[0] | r[1] << 8);
        const char *kind = !(type & 0x10) ? "EXT_ADV"
            : type & 0x08 ? "SCAN_RSP"
            : type & 0x04 ? "ADV_DIRECT_IND"
            : (type & 0x03) == 0x03 ? "ADV_IND"
            : type & 0x02 ? "ADV_SCAN_IND" : "ADV_NONCONN_IND";
        _report(kind, r + 3, (int8_t)r[13], r + 24, len);
        at += 24 + len;
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
    if (handled && len >= 3) {
        bus_trace_text("BLE ATT %s %s 0x%04x %s%s", to_phone ? "->" : "<-", name, pdu[1] | pdu[2] << 8, hex, text);
    } else {
        bus_trace_text("BLE ATT %s %s %s%s", to_phone ? "->" : "<-", name, hex, text);
    }
}

// Pairing Failed's reasons, as a person reads them.
static const char *_smp_reason(uint8_t reason) {
    static const char *const reasons[] = {
        "", "passkey entry failed", "OOB not available", "authentication requirements",
        "confirm value failed", "pairing not supported", "encryption key size",
        "command not supported", "unspecified reason", "repeated attempts", "invalid parameters",
        "DHKey check failed", "numeric comparison failed", "BR/EDR pairing in progress",
        "cross-transport key derivation not allowed",
    };
    return reason < sizeof(reasons) / sizeof(reasons[0]) ? reasons[reason] : "unknown";
}

// A Pairing Request or Response's IO capability, Bonding flag and maximum key size.
static void _smp_features(char *out, size_t cap, const uint8_t *pdu) {
    static const char *const io[] = { "display", "yes-no", "keyboard", "none", "keyboard-display" };
    snprintf(out, cap, "io=%s%s%s%s key=%u", pdu[1] < 5 ? io[pdu[1]] : "?",
        (pdu[3] & 0x03) == 0x01 ? " bond" : "", pdu[3] & 0x04 ? " mitm" : "", pdu[3] & 0x08 ? " sc" : "",
        pdu[4]);
}

// An SMP PDU as a trace line: what it is, never the keys or random values it carries.
static void _trace_smp(bool to_phone, const uint8_t *pdu, size_t len) {
    if (len == 0 || !bus_trace_on()) {
        return;
    }
    const char *dir = to_phone ? "->" : "<-";
    char what[64];
    switch (pdu[0]) {
        case 0x01:
        case 0x02:
            if (len >= 7) {
                _smp_features(what, sizeof(what), pdu);
                bus_trace_text("BLE PAIR %s %s %s", dir, pdu[0] == 0x01 ? "REQUEST" : "RESPONSE", what);
            }
            break;
        case 0x03:
            bus_trace_text("BLE PAIR %s CONFIRM", dir);
            break;
        case 0x04:
            bus_trace_text("BLE PAIR %s RANDOM", dir);
            break;
        case 0x05:
            if (len >= 2) {
                bus_trace_text("BLE PAIR FAILED 0x%02x %s", pdu[1], _smp_reason(pdu[1]));
            }
            break;
        case 0x06:
            bus_trace_text("BLE PAIR %s ENCRYPTION_INFORMATION", dir);
            break;
        case 0x07:
            if (len >= 3) {
                bus_trace_text("BLE PAIR %s CENTRAL_IDENTIFICATION ediv=0x%04x", dir, pdu[1] | pdu[2] << 8);
            }
            break;
        case 0x08:
            bus_trace_text("BLE PAIR %s IDENTITY_INFORMATION", dir);
            break;
        case 0x09:
            if (len >= 8) {
                bus_trace_text("BLE PAIR %s IDENTITY_ADDRESS %02x:%02x:%02x:%02x:%02x:%02x", dir,
                    pdu[7], pdu[6], pdu[5], pdu[4], pdu[3], pdu[2]);
            }
            break;
        case 0x0b:
            if (len >= 2) {
                bus_trace_text("BLE PAIR %s SECURITY_REQUEST%s", dir, (pdu[1] & 0x03) == 0x01 ? " bond" : "");
            }
            break;
        default:
            bus_trace_text("BLE PAIR %s 0x%02x", dir, pdu[0]);
            break;
    }
}

// The last LE Long Term Key Request's EDIV, and whether it was the STK's (EDIV and Rand 0).
static uint16_t _ltk_ediv;
static bool _ltk_stk;

static void _command(const uint8_t *h4, size_t len) {
    uint16_t opcode = (uint16_t)(h4[1] | h4[2] << 8);
    const uint8_t *p = h4 + 4;
    size_t n = len - 4;
    switch (opcode) {
        case 0x0c03:            // Reset
            _state.advertising = _state.connected = false;
            _state.link_count = 0;
            _scan(false);
            break;
        case 0x200b:            // LE Set Scan Parameters: active or passive
            if (n >= 1) {
                _scan_active = p[0] == 0x01;
            }
            break;
        case 0x2041:            // LE Set Extended Scan Parameters: the first PHY's type
            if (n >= 4) {
                _scan_active = p[3] == 0x01;
            }
            break;
        case 0x200c:            // LE Set Scan Enable
        case 0x2042:            // LE Set Extended Scan Enable
            _scan(n >= 1 && p[0] != 0);
            break;
        case 0x200e:            // LE Create Connection Cancel
            bus_trace_text("BLE CONNECT cancel");
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
        // The extended commands of a controller with extended advertising, for advertising set 0
        // with legacy PDUs, as ble_hci sends them.
        case 0x2035:            // LE Set Advertising Set Random Address
            if (n >= 7) {
                memcpy(_random_address, p + 1, 6);
            }
            break;
        case 0x2036:            // LE Set Extended Advertising Parameters: properties, own address type
            if (n >= 11) {
                uint16_t props = (uint16_t)(p[1] | p[2] << 8);
                _adv_type = props & 0x01 ? (props & 0x04 ? 0x01 : 0x00) : props & 0x02 ? 0x02 : 0x03;
                _own_address_type = p[10];
            }
            break;
        case 0x2037:            // LE Set Extended Advertising Data
        case 0x2038:            // LE Set Extended Scan Response Data
            if (n >= 4 && p[3] <= 31 && 4u + p[3] <= n) {
                memcpy(opcode == 0x2037 ? _state.data : _state.scan_response, p + 4, p[3]);
                *(opcode == 0x2037 ? &_state.data_len : &_state.scan_response_len) = p[3];
            }
            break;
        case 0x2039:            // LE Set Extended Advertising Enable
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
        case 0x201a:            // LE Long Term Key Request Reply
            if (!_ltk_stk) {
                bus_trace_text("BLE LTK ediv=0x%04x found", _ltk_ediv);
            }
            break;
        case 0x201b:            // LE Long Term Key Request Negative Reply: the peer hears 0x06
            bus_trace_text("BLE LTK ediv=0x%04x not found", _ltk_ediv);
            bus_trace_text("BLE ENCRYPT FAILED 0x06 key missing");
            break;
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
    } else if (h4[1] == 0x3e && n >= 12 && (p[0] == 0x01 || p[0] == 0x0a)) {
        // LE (Enhanced) Connection Complete: the peer is connected; a central's connection leaves
        // the advertising on, a peripheral's stops it.
        bool central = p[4] == 0x00;
        if (p[1] != 0) {
            bus_trace_text("BLE CONNECT %02x:%02x:%02x:%02x:%02x:%02x failed 0x%02x", p[11], p[10], p[9], p[8],
                p[7], p[6], p[1]);
            return;
        }
        _state.connected = true;
        if (!central) {
            _state.advertising = false;
        }
        memcpy(_state.peer, p + 6, 6);
        if (central) {
            // Its name, as a scan found it, or its address.
            const char *name = _name_of(p + 6);
            if (name != NULL) {
                snprintf(_state.central_name, sizeof(_state.central_name), "%s", name);
            } else {
                snprintf(_state.central_name, sizeof(_state.central_name), "%02x:%02x:%02x:%02x:%02x:%02x",
                    p[11], p[10], p[9], p[8], p[7], p[6]);
            }
        }
        if (_state.link_count < BLE_HOST_LINKS) {
            ble_host_link_t *link = &_state.links[_state.link_count++];
            link->handle = (uint16_t)(p[2] | p[3] << 8) & 0x0fff;
            link->central = central;
            link->encrypted = false;
            memcpy(link->peer, p + 6, 6);
        }
        bus_trace_text("BLE CONNECT %02x:%02x:%02x:%02x:%02x:%02x%s", p[11], p[10], p[9], p[8], p[7], p[6],
            central ? " central" : "");
    } else if (h4[1] == 0x3e && n >= 2 && p[0] == 0x02) {
        _reports(p, n);
    } else if (h4[1] == 0x3e && n >= 2 && p[0] == 0x0d) {
        _extended_reports(p, n);
    } else if (h4[1] == 0x05 && n >= 4 && p[0] == 0) {
        ble_host_link_t *link = _link((uint16_t)(p[1] | p[2] << 8) & 0x0fff);
        if (link != NULL) {
            ble_host_link_t *end = &_state.links[_state.link_count--];
            memmove(link, link + 1, (size_t)(end - (link + 1)) * sizeof(*link));
        }
        _state.connected = _state.link_count > 0;
        bus_trace_text("BLE DISCONNECT 0x%02x", p[3]);
    } else if ((h4[1] == 0x08 && n >= 4) || (h4[1] == 0x30 && n >= 3)) {
        // Encryption Change (its on or off), Encryption Key Refresh Complete (on). The key's size
        // is the host's: it set it as it answered the key request, before this event comes (this
        // runs on the VM thread, as the host's poll reads the event).
        const uint16_t handle = (uint16_t)(p[1] | p[2] << 8) & 0x0fff;
        ble_host_link_t *link = _link(handle);
        bleio_connection_internal_t *connection = bleio_conn_handle_to_connection(handle);
        if (p[0] != 0) {
            bus_trace_text("BLE ENCRYPT FAILED 0x%02x", p[0]);
        } else if (h4[1] == 0x30) {
            bus_trace_text("BLE ENCRYPTED refresh");
        } else if (p[3] != 0) {
            bus_trace_text("BLE ENCRYPTED key=%u", connection != NULL ? connection->key_size : 0);
        }
        if (link != NULL && p[0] == 0) {
            link->encrypted = h4[1] == 0x30 || p[3] != 0;
            link->key_size = connection != NULL ? connection->key_size : 0;
        }
    } else if (h4[1] == 0x3e && n >= 13 && p[0] == 0x05) {
        // LE Long Term Key Request: the peer encrypts with the key of this EDIV and Rand.
        static const uint8_t zero[8] = { 0 };
        _ltk_ediv = (uint16_t)(p[11] | p[12] << 8);
        _ltk_stk = _ltk_ediv == 0 && memcmp(p + 3, zero, sizeof(zero)) == 0;
    }
}

// The bond store as it was last read or written.
static bonding_store_t _bonds;
static bool _have_bonds;

static void _trace_bond(const bonding_record_t *r) {
    const uint8_t *a = r->peer.a.val;
    bus_trace_text("BLE BOND saved %s%02x:%02x:%02x:%02x:%02x:%02x",
        (r->flags & BONDING_PEER_IRK) ? "identity " : "", a[5], a[4], a[3], a[2], a[1], a[0]);
}

void ble_host_bonds_stored(const uint8_t *blob, size_t len, bool written) {
    if (len != sizeof(bonding_store_t)) {
        return;
    }
    const bonding_store_t *store = (const bonding_store_t *)blob;
    pthread_mutex_lock(&_lock);
    if (written) {
        // A bond new or made again (its key changed) is saved; one gone is erased.
        int erased = 0;
        for (size_t i = 0; i < BONDING_MAX; i++) {
            const bonding_record_t *was = &_bonds.records[i], *is = &store->records[i];
            bool had = _have_bonds && (was->flags & BONDING_IN_USE);
            if ((is->flags & BONDING_IN_USE) && (!had || memcmp(was->ltk, is->ltk, sizeof(is->ltk)) != 0 ||
                                                 memcmp(&was->peer, &is->peer, sizeof(is->peer)) != 0)) {
                _trace_bond(is);
            } else if (had && !(is->flags & BONDING_IN_USE)) {
                erased++;
            }
        }
        if (erased > 0) {
            bus_trace_text("BLE BOND erased %d", erased);
        }
    }
    memcpy(&_bonds, blob, sizeof(_bonds));
    _have_bonds = true;
    // The bonds of the controller's identity, as the adapter found it at its last enable.
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    uint8_t count = 0;
    for (size_t i = 0; i < BONDING_MAX; i++) {
        const bonding_record_t *r = &store->records[i];
        count += (r->flags & BONDING_IN_USE) &&
            (!adapter->identity_known || memcmp(&r->own, &adapter->identity, sizeof(r->own)) == 0);
    }
    _state.bonds = count;
    pthread_mutex_unlock(&_lock);
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
    } else if (h4[0] == 0x02 && len >= 9 && (h4[2] >> 4 & 0x3) != 0x1 && (h4[7] | h4[8] << 8) == 0x0006) {
        // ... on the SMP channel.
        _trace_smp(to_controller, h4 + 9, len - 9);
    }
    pthread_mutex_unlock(&_lock);
}

void ble_host_state(ble_host_state_t *state) {
    pthread_mutex_lock(&_lock);
    *state = _state;
    pthread_mutex_unlock(&_lock);
}
