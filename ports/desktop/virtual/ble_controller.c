// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// A Bluetooth LE controller in the lab, for _bleio's HCI host stack: it answers the commands the
// host sends as a real controller does (the values a Pico 2 W's CYW43439 gave). Around it is an
// air the control API fills: peers that advertise, which the program scans for and connects to,
// and the phone, which connects to what the program advertises. Each connection is a link of its
// own (its handle, the program's role, the peer, encryption, its reassembly), and every L2CAP
// frame on it, of any channel, goes to the events the control API streams. It has no time of
// its own: nothing happens until the host or the API does something.

#include "ble_controller.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "shared-bindings/os/__init__.h"

#include "desktop_port.h"

#define H4_COMMAND 0x01
#define H4_ACL 0x02
#define H4_EVENT 0x04

#define EVT_DISCONNECTION_COMPLETE 0x05
#define EVT_COMMAND_COMPLETE 0x0e
#define EVT_COMMAND_STATUS 0x0f
#define EVT_NUMBER_OF_COMPLETED_PACKETS 0x13
#define EVT_LE_META 0x3e
#define EVT_VENDOR 0xff

#define LE_CONNECTION_COMPLETE 0x01
#define LE_ADVERTISING_REPORT 0x02
#define LE_ENHANCED_CONNECTION_COMPLETE 0x0a
#define LE_EXTENDED_ADVERTISING_REPORT 0x0d

#define L2CAP_CID_ATT 0x0004

#define ERR_UNKNOWN_COMMAND 0x01
#define ERR_UNKNOWN_CONNECTION 0x02
#define ERR_CONNECTION_LIMIT 0x09
#define ERR_COMMAND_DISALLOWED 0x0c
#define ERR_REMOTE_USER 0x13
#define ERR_LOCAL_HOST 0x16

// Advertising PDU types, as LE Advertising Report gives them.
#define ADV_IND 0x00
#define ADV_DIRECT_IND 0x01
#define ADV_SCAN_IND 0x02
#define ADV_NONCONN_IND 0x03
#define SCAN_RSP 0x04

static pthread_mutex_t _lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t _event_added = PTHREAD_COND_INITIALIZER;

// The bytes on their way to the host.
static uint8_t _out[65536];
static size_t _out_head;
static size_t _out_count;

// CIRCUITPY_BLE_VIRTUAL_FAULT=no-public-address: a controller without a public address, as
// Zephyr's on an nRF. =extended: one with LE Extended Advertising (feature bit 12), as Zephyr's
// on an nRF52840, which refuses the legacy advertising, scanning and connecting commands.
static bool _no_public_address;
static bool _extended;

// The lab's addresses, least significant byte first: a public one, 02:dc and four bytes of a hash
// of the drive's path (two drives, two addresses), and a static random one for a host that asks
// for the controller's (Zephyr's vendor command), c2:dc and the same four.
static uint8_t _public_address[6];
static uint8_t _static_address[6];
static bool _have_addresses;

// The LE events the host takes (LE Set Event Mask): by default the first five.
static uint64_t _le_mask;

// What the program advertises: ADV_IND and ADV_DIRECT_IND (0, 1) take a connection.
static uint8_t _adv_type;
static bool _advertising;
static uint8_t _adv_data[31];
static uint8_t _adv_data_len;
static uint8_t _scan_response[31];
static uint8_t _scan_response_len;
static uint8_t _own_address_type;   // of the advertising: 0 public, 1 random
static uint8_t _random_address[6];  // what LE Set Random Address set

// What the program scans for.
static bool _scanning;
static bool _scan_active;
static bool _filter_duplicates;

// A connection the program asked for (LE Create Connection) to a peer not there yet.
static bool _initiating;
static uint8_t _initiate_address[6];
static uint8_t _initiate_type;
static uint8_t _initiate_own_type;
static uint16_t _initiate_interval;

// The phone's address, sent as a random one: 0x7c's top bits (01) make it a resolvable private
// address by its form, as a phone's is, with no identity behind it.
static const uint8_t _phone_address[6] = { 0x89, 0x37, 0x1c, 0xc9, 0x10, 0x7c };

#define PDU_MAX 512

// The peers in the air around the program: they advertise (or are silent, once connected).
#define PEER_COUNT 16
typedef struct {
    bool used;
    bool advertising;
    bool connectable;
    uint8_t address[6];
    bool random;
    uint8_t data[31];
    uint8_t data_len;
    uint8_t scan_response[31];
    uint8_t scan_response_len;
    int8_t rssi;
    bool reported;              // in this scan (with Filter Duplicates on, once per scan)
} peer_t;
static peer_t _peers[PEER_COUNT];

// The links: handles 0x0040 on, as the CYW43439 numbers its connections.
#define LINK_COUNT 4
#define LINK_HANDLE(i) (0x0040 + (i))
#define ROLE_CENTRAL 0x00       // the program's role, as LE Connection Complete gives it
#define ROLE_PERIPHERAL 0x01
typedef struct {
    bool open;
    uint8_t role;
    uint8_t peer[6];
    uint8_t peer_type;          // 0 public, 1 random
    uint8_t local[6];           // the program's address on it
    uint8_t local_type;
    bool encrypted;
    uint8_t frame[PDU_MAX + 4]; // the L2CAP frame from the host being reassembled
    size_t frame_have;
} link_t;
static link_t _links[LINK_COUNT];
static int _phone = -1;         // the phone's link, or -1

// ATT PDUs the program sent the phone, oldest first.
#define PDU_COUNT 256
static uint8_t _pdus[PDU_COUNT][PDU_MAX];
static uint16_t _pdu_len[PDU_COUNT];
static size_t _pdu_head;
static size_t _pdu_count;

// The events, a ring of the last EVENT_COUNT: _event_end is the number of the next one.
#define EVENT_COUNT 256
static ble_event_t _events[EVENT_COUNT];
static uint64_t _event_end;

static void _put(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len && _out_count < sizeof(_out); i++) {
        _out[(_out_head + _out_count++) % sizeof(_out)] = data[i];
    }
}

static void _event(uint8_t code, const uint8_t *params, uint8_t len) {
    uint8_t head[3] = { H4_EVENT, code, len };
    _put(head, sizeof(head));
    _put(params, len);
}

// Command Complete: one more command may come, the opcode, then the return parameters.
static void _complete(uint16_t opcode, const uint8_t *ret, uint8_t len) {
    uint8_t params[3 + 255];
    params[0] = 1;
    params[1] = opcode & 0xff;
    params[2] = opcode >> 8;
    memcpy(params + 3, ret, len);
    _event(EVT_COMMAND_COMPLETE, params, (uint8_t)(3 + len));
}

static void _status(uint16_t opcode, uint8_t status) {
    uint8_t params[4] = { status, 1, opcode & 0xff, opcode >> 8 };
    _event(EVT_COMMAND_STATUS, params, sizeof(params));
}

static void _ok(uint16_t opcode) {
    uint8_t status = 0;
    _complete(opcode, &status, 1);
}

static void _refuse(uint16_t opcode, uint8_t status) {
    _complete(opcode, &status, 1);
}

// ---- the events ----

static ble_event_t *_new_event(uint8_t kind) {
    ble_event_t *e = &_events[_event_end % EVENT_COUNT];
    memset(e, 0, sizeof(*e));
    e->seq = _event_end++;
    e->kind = kind;
    pthread_cond_broadcast(&_event_added);
    return e;
}

// The program's own address of a type: its public one or the random one the host set.
static const uint8_t *_own(uint8_t type) {
    return type == 0x00 || type == 0x02 ? _public_address : _random_address;
}

static void _adv_event(bool on) {
    ble_event_t *e = _new_event(BLE_EVENT_ADV);
    e->on = on;
    e->connectable = _adv_type == ADV_IND || _adv_type == ADV_DIRECT_IND;
    memcpy(e->address, _own(_own_address_type), 6);
    e->address_random = _own_address_type & 1;
    memcpy(e->data, _adv_data, _adv_data_len);
    e->len = _adv_data_len;
    memcpy(e->data + 31, _scan_response, _scan_response_len);
    e->scan_response_len = _scan_response_len;
}

static void _set_advertising(bool on) {
    if (on != _advertising) {
        _advertising = on;
        _adv_event(on);
    }
}

static void _link_event(uint8_t kind, int i) {
    ble_event_t *e = _new_event(kind);
    e->handle = LINK_HANDLE(i);
    e->role = _links[i].role;
    memcpy(e->address, _links[i].peer, 6);
    e->address_random = _links[i].peer_type & 1;
    memcpy(e->local, _links[i].local, 6);
    e->local_random = _links[i].local_type & 1;
    e->encrypted = _links[i].encrypted;
}

// ---- the links ----

// The link of a handle, -1 when none is open there.
static int _link_of(uint16_t handle) {
    int i = (int)handle - LINK_HANDLE(0);
    return i >= 0 && i < LINK_COUNT && _links[i].open ? i : -1;
}

// The open link to a peer, -1 when there is none.
static int _link_to(const uint8_t address[6]) {
    for (int i = 0; i < LINK_COUNT; i++) {
        if (_links[i].open && memcmp(_links[i].peer, address, 6) == 0) {
            return i;
        }
    }
    return -1;
}

// A new link, -1 when all are in use.
static int _link_open(uint8_t role, const uint8_t peer[6], uint8_t peer_type, uint8_t local_type) {
    for (int i = 0; i < LINK_COUNT; i++) {
        if (!_links[i].open) {
            memset(&_links[i], 0, sizeof(_links[i]));
            _links[i].open = true;
            _links[i].role = role;
            memcpy(_links[i].peer, peer, 6);
            _links[i].peer_type = peer_type & 1;
            memcpy(_links[i].local, _own(local_type), 6);
            _links[i].local_type = local_type & 1;
            return i;
        }
    }
    return -1;
}

// LE Connection Complete, or LE Enhanced Connection Complete when the host takes that (a host
// that unmasks it gets it in place of the other, as from a Bluetooth 5 controller). interval in
// units of 1.25 ms; no latency, a 2 s supervision timeout.
static void _connection_complete(uint8_t status, uint16_t handle, uint8_t role, uint8_t peer_type,
    const uint8_t peer[6], uint16_t interval) {
    bool enhanced = _le_mask & (1u << 9);
    if (!enhanced && !(_le_mask & 1)) {
        return;
    }
    uint8_t p[31] = { enhanced ? LE_ENHANCED_CONNECTION_COMPLETE : LE_CONNECTION_COMPLETE, status,
                      handle & 0xff, handle >> 8, role, peer_type };
    memcpy(p + 6, peer, 6);
    size_t at = enhanced ? 24 : 12;     // the enhanced one has the local and peer RPAs (none) first
    p[at] = interval & 0xff;
    p[at + 1] = interval >> 8;
    p[at + 4] = 0xc8;
    _event(EVT_LE_META, p, (uint8_t)(at + 7));
}

// The link ends: Disconnection Complete to the host (unless a reset ended it) and the event.
static void _link_close(int i, uint8_t reason, bool tell_host) {
    if (i < 0 || !_links[i].open) {
        return;
    }
    ble_event_t *e;
    _link_event(BLE_EVENT_DISCONNECTED, i);
    e = &_events[(_event_end - 1) % EVENT_COUNT];
    e->reason = reason;
    _links[i].open = false;
    if (i == _phone) {
        _phone = -1;
    }
    if (tell_host) {
        uint8_t params[4] = { 0, LINK_HANDLE(i) & 0xff, LINK_HANDLE(i) >> 8, reason };
        _event(EVT_DISCONNECTION_COMPLETE, params, sizeof(params));
    }
}

// A frame to the program on a link: an L2CAP frame in ACL packets of at most 251 bytes, as LE's
// are, the first flagged as a start, the rest as continuations.
static void _to_host(int i, uint16_t cid, const uint8_t *data, size_t len) {
    uint8_t frame[4 + PDU_MAX] = { (uint8_t)len, (uint8_t)(len >> 8), cid & 0xff, cid >> 8 };
    memcpy(frame + 4, data, len);
    uint16_t handle = LINK_HANDLE(i);
    for (size_t at = 0; at < len + 4; at += 251) {
        size_t n = len + 4 - at < 251 ? len + 4 - at : 251;
        uint8_t head[5] = { H4_ACL, handle & 0xff, (handle >> 8) | (at == 0 ? 0x20 : 0x10), (uint8_t)n, (uint8_t)(n >> 8) };
        _put(head, sizeof(head));
        _put(frame + at, n);
    }
}

// A peer connects to what the program advertises: the program is the peripheral.
static int _peripheral_link(const uint8_t peer[6], uint8_t peer_type) {
    if (!_advertising || (_adv_type != ADV_IND && _adv_type != ADV_DIRECT_IND) || _link_to(peer) >= 0) {
        return -1;
    }
    int i = _link_open(ROLE_PERIPHERAL, peer, peer_type, _own_address_type);
    if (i < 0) {
        return -1;
    }
    _set_advertising(false);
    _connection_complete(0, LINK_HANDLE(i), ROLE_PERIPHERAL, _links[i].peer_type, peer, 0x18);
    // A vendor event, as the CYW43439 sends right after a connection: a host must let it pass.
    uint8_t vendor[5] = { 0x55, 0x00, 0x00, LINK_HANDLE(i) & 0xff, LINK_HANDLE(i) >> 8 };
    _event(EVT_VENDOR, vendor, sizeof(vendor));
    _link_event(BLE_EVENT_CONNECTED, i);
    return i;
}

// ---- the peers ----

static peer_t *_peer(const uint8_t address[6]) {
    for (int i = 0; i < PEER_COUNT; i++) {
        if (_peers[i].used && memcmp(_peers[i].address, address, 6) == 0) {
            return &_peers[i];
        }
    }
    return NULL;
}

static uint8_t _adv_kind(const peer_t *p) {
    return p->connectable ? ADV_IND : p->scan_response_len > 0 ? ADV_SCAN_IND : ADV_NONCONN_IND;
}

// One advertising report of a peer: the legacy event, or the extended one for a host that scans
// with the extended commands.
static void _report_one(const peer_t *p, uint8_t kind, const uint8_t *data, uint8_t len) {
    uint8_t r[64];
    if (_extended) {
        if (!(_le_mask & (1u << 12))) {
            return;
        }
        // Event type bits: connectable, scannable, directed, scan response, legacy.
        static const uint16_t legacy[] = { 0x13, 0x15, 0x12, 0x10, 0x1b };
        uint16_t type = kind == SCAN_RSP && p->connectable ? 0x1b : kind == SCAN_RSP ? 0x1a : legacy[kind];
        r[0] = LE_EXTENDED_ADVERTISING_REPORT;
        r[1] = 1;
        r[2] = type & 0xff;
        r[3] = type >> 8;
        r[4] = p->random ? 1 : 0;
        memcpy(r + 5, p->address, 6);
        r[11] = 0x01;           // primary PHY 1M
        r[12] = 0x00;           // no secondary
        r[13] = 0xff;           // no SID
        r[14] = 0x7f;           // TX power not available
        r[15] = (uint8_t)p->rssi;
        memset(r + 16, 0, 9);   // periodic interval, direct address type and address
        r[25] = len;
        memcpy(r + 26, data, len);
        _event(EVT_LE_META, r, (uint8_t)(26 + len));
        return;
    }
    if (!(_le_mask & (1u << 1))) {
        return;
    }
    r[0] = LE_ADVERTISING_REPORT;
    r[1] = 1;
    r[2] = kind;
    r[3] = p->random ? 1 : 0;
    memcpy(r + 4, p->address, 6);
    r[10] = len;
    memcpy(r + 11, data, len);
    r[11 + len] = (uint8_t)p->rssi;
    _event(EVT_LE_META, r, (uint8_t)(12 + len));
}

// A peer's reports to a scanning program: its advertisement and, to an active scan, its scan
// response; with Filter Duplicates on, once per scan.
static void _report(peer_t *p) {
    if (!_scanning || !p->advertising || (_filter_duplicates && p->reported)) {
        return;
    }
    p->reported = true;
    _report_one(p, _adv_kind(p), p->data, p->data_len);
    if (_scan_active && p->scan_response_len > 0) {
        _report_one(p, SCAN_RSP, p->scan_response, p->scan_response_len);
    }
}

// The program connects to a peer that is there: the link, Connection Complete, the event. The
// peer stops advertising, as an advertiser does once connected.
static void _central_link(peer_t *p) {
    _initiating = false;
    int i = _link_open(ROLE_CENTRAL, p->address, p->random, _initiate_own_type);
    if (i < 0) {
        _connection_complete(ERR_CONNECTION_LIMIT, 0, ROLE_CENTRAL, p->random, p->address, 0);
        return;
    }
    p->advertising = false;
    _connection_complete(0, LINK_HANDLE(i), ROLE_CENTRAL, _links[i].peer_type, p->address, _initiate_interval);
    _link_event(BLE_EVENT_CONNECTED, i);
}

// A peer that may complete the connection the program asked for.
static void _try_initiate(peer_t *p) {
    if (_initiating && p->advertising && p->connectable && memcmp(p->address, _initiate_address, 6) == 0) {
        _central_link(p);
    }
}

// ---- the addresses ----

static void _addresses(void) {
    if (_have_addresses) {
        return;
    }
    _have_addresses = true;
    unsigned v[6];
    const char *set = getenv("CIRCUITPY_BLE_ADDRESS");
    if (set != NULL && sscanf(set, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            _public_address[5 - i] = (uint8_t)v[i];
        }
    } else {
        // FNV-1a over the drive's path, which is absolute by the time the host resets the controller.
        uint32_t h = 2166136261u;
        for (const char *c = desktop_options.drive_path; *c != '\0'; c++) {
            h = (h ^ (uint8_t)*c) * 16777619u;
        }
        const uint8_t address[6] = { h & 0xff, (h >> 8) & 0xff, (h >> 16) & 0xff, h >> 24, 0xdc, 0x02 };
        memcpy(_public_address, address, 6);
    }
    memcpy(_static_address, _public_address, 6);
    _static_address[5] |= 0xc0;
}

// ---- the commands ----

static void _reset_state(void) {
    _set_advertising(false);
    if (_scanning) {
        _scanning = false;
        _new_event(BLE_EVENT_SCAN);
    }
    _initiating = false;
    // The links end; the host knows (it reset), the peers learn it from the events.
    for (int i = 0; i < LINK_COUNT; i++) {
        _link_close(i, ERR_LOCAL_HOST, false);
    }
    _phone = -1;
    _le_mask = 0x1f;
}

static void _scan_enable(bool on, bool filter_duplicates) {
    if (on == _scanning) {
        return;
    }
    _scanning = on;
    _filter_duplicates = filter_duplicates;
    ble_event_t *e = _new_event(BLE_EVENT_SCAN);
    e->on = on;
    e->active = _scan_active;
    if (on) {
        for (int i = 0; i < PEER_COUNT; i++) {
            _peers[i].reported = false;
        }
        for (int i = 0; i < PEER_COUNT; i++) {
            if (_peers[i].used) {
                _report(&_peers[i]);
            }
        }
    }
}

// LE Create Connection and LE Extended Create Connection: the peer and how.
static void _create_connection(uint16_t opcode, uint8_t peer_type, const uint8_t *peer, uint8_t own_type, uint16_t interval) {
    if (_initiating) {
        _status(opcode, ERR_COMMAND_DISALLOWED);
        return;
    }
    _status(opcode, 0);
    _initiating = true;
    memcpy(_initiate_address, peer, 6);
    _initiate_type = peer_type;
    _initiate_own_type = own_type;
    _initiate_interval = interval;
    for (int i = 0; i < PEER_COUNT && _initiating; i++) {
        if (_peers[i].used) {
            _try_initiate(&_peers[i]);
        }
    }
}

static bool _legacy_refused(uint16_t opcode) {
    switch (opcode) {
        case 0x2006:
        case 0x2008:
        case 0x2009:
        case 0x200a:
        case 0x200b:
        case 0x200c:
        case 0x200d:
            return _extended;
        default:
            return false;
    }
}

static void _command(uint16_t opcode, const uint8_t *p, uint8_t len) {
    uint8_t ret[32] = { 0 };
    if (_legacy_refused(opcode)) {
        // A controller with extended advertising takes the extended commands only, as Zephyr's.
        if (opcode == 0x200d) {
            _status(opcode, ERR_COMMAND_DISALLOWED);
        } else {
            _refuse(opcode, ERR_COMMAND_DISALLOWED);
        }
        return;
    }
    switch (opcode) {
        case 0x0c03:            // Reset
            _addresses();
            _reset_state();
            _ok(opcode);
            break;
        case 0x0c01:            // Set Event Mask
            _ok(opcode);
            break;
        case 0x2001:            // LE Set Event Mask
            if (len >= 8) {
                _le_mask = 0;
                for (int i = 7; i >= 0; i--) {
                    _le_mask = _le_mask << 8 | p[i];
                }
            }
            _ok(opcode);
            break;
        case 0x2005:            // LE Set Random Address
            if (len >= 6) {
                memcpy(_random_address, p, 6);
            }
            _ok(opcode);
            break;
        case 0x2008:            // LE Set Advertising Data
        case 0x2009:            // LE Set Scan Response Data
            if (len >= 1 && p[0] <= 31 && p[0] < len) {
                memcpy(opcode == 0x2008 ? _adv_data : _scan_response, p + 1, p[0]);
                *(opcode == 0x2008 ? &_adv_data_len : &_scan_response_len) = p[0];
            }
            _ok(opcode);
            break;
        case 0x200b:            // LE Set Scan Parameters: active or passive
            if (len >= 1) {
                _scan_active = p[0] == 0x01;
            }
            _ok(opcode);
            break;
        case 0x200c:            // LE Set Scan Enable
            _ok(opcode);
            _scan_enable(len >= 1 && p[0] != 0, len >= 2 && p[1] != 0);
            break;
        case 0x200d:            // LE Create Connection
            if (len < 25) {
                _status(opcode, 0x12);
                break;
            }
            _create_connection(opcode, p[5], p + 6, p[12], (uint16_t)(p[13] | p[14] << 8));
            break;
        case 0x200e:            // LE Create Connection Cancel
            if (!_initiating) {
                _refuse(opcode, ERR_COMMAND_DISALLOWED);
                break;
            }
            _initiating = false;
            _ok(opcode);
            _connection_complete(ERR_UNKNOWN_CONNECTION, 0, ROLE_CENTRAL, _initiate_type, _initiate_address, 0);
            break;
        case 0x1001:            // Read Local Version Information: Bluetooth 5.2, no manufacturer
            ret[1] = 0x0b;
            ret[4] = 0x0b;
            ret[5] = 0xff;
            ret[6] = 0xff;
            _complete(opcode, ret, 9);
            break;
        case 0x1009:            // Read BD_ADDR
            _addresses();
            if (!_no_public_address) {
                memcpy(ret + 1, _public_address, 6);
            }
            _complete(opcode, ret, 7);
            break;
        case 0x2002:            // LE Read Buffer Size: 251 bytes, 8 packets
            ret[1] = 251;
            ret[3] = 8;
            _complete(opcode, ret, 4);
            break;
        case 0x2003:            // LE Read Local Supported Features: encryption; extended advertising in that profile
            ret[1] = 0x01;
            ret[2] = _extended ? 0x10 : 0x00;
            _complete(opcode, ret, 9);
            break;
        case 0x2006:            // LE Set Advertising Parameters
            if (len >= 6) {
                _adv_type = p[4];
                _own_address_type = p[5];
            }
            _ok(opcode);
            break;
        case 0x200a:            // LE Set Advertising Enable
            if (len >= 1 && p[0] == 0 && !_advertising && _phone >= 0) {
                // Stopping what a connection already stopped: Command Disallowed, as a CYW43439
                _refuse(opcode, ERR_COMMAND_DISALLOWED);
                break;
            }
            _ok(opcode);
            _set_advertising(len >= 1 && p[0] != 0);
            break;
        case 0x0406: {          // Disconnect: the host ends a connection
            int i = len >= 2 ? _link_of((uint16_t)(p[0] | p[1] << 8) & 0x0fff) : -1;
            _status(opcode, i >= 0 ? 0 : ERR_UNKNOWN_CONNECTION);
            _link_close(i, ERR_LOCAL_HOST, true);
            break;
        }
        case 0x2018:            // LE Rand
            common_hal_os_urandom(ret + 1, 8);
            _complete(opcode, ret, 9);
            break;
        case 0xfc09:            // Zephyr's Read Static Addresses: one, with a zero identity root
            _addresses();
            ret[1] = 1;
            memcpy(ret + 2, _static_address, 6);
            _complete(opcode, ret, 2 + 6 + 16);
            break;
        default:
            ret[0] = ERR_UNKNOWN_COMMAND;
            _complete(opcode, ret, 1);
            break;
    }
}

// ACL data from the host on a link: an L2CAP frame, maybe in fragments, to the events (any
// channel), and an ATT one on the phone's link to the phone's queue too. Each packet is
// acknowledged on its handle, so the host's credit comes back.
static void _acl(const uint8_t *p, size_t len) {
    uint16_t handle = (uint16_t)(p[1] | p[2] << 8) & 0x0fff;
    uint16_t flags = p[2] >> 4;
    size_t n = len - 5;
    uint8_t done[5] = { 1, handle & 0xff, handle >> 8, 1, 0 };
    _event(EVT_NUMBER_OF_COMPLETED_PACKETS, done, sizeof(done));
    int i = _link_of(handle);
    if (i < 0) {
        return;
    }
    link_t *link = &_links[i];
    if ((flags & 0x3) != 0x1) {
        link->frame_have = 0;   // the first fragment of a frame
    }
    if (link->frame_have + n <= sizeof(link->frame)) {
        memcpy(link->frame + link->frame_have, p + 5, n);
        link->frame_have += n;
    }
    if (link->frame_have < 4) {
        return;
    }
    size_t frame_len = (size_t)(link->frame[0] | link->frame[1] << 8);
    uint16_t cid = (uint16_t)(link->frame[2] | link->frame[3] << 8);
    if (link->frame_have < 4 + frame_len) {
        return;
    }
    if (frame_len <= BLE_EVENT_DATA_MAX) {
        _link_event(BLE_EVENT_L2CAP, i);
        ble_event_t *e = &_events[(_event_end - 1) % EVENT_COUNT];
        e->cid = cid;
        e->len = (uint16_t)frame_len;
        memcpy(e->data, link->frame + 4, frame_len);
    }
    if (i == _phone && cid == L2CAP_CID_ATT && frame_len <= PDU_MAX && _pdu_count < PDU_COUNT) {
        size_t slot = (_pdu_head + _pdu_count++) % PDU_COUNT;
        memcpy(_pdus[slot], link->frame + 4, frame_len);
        _pdu_len[slot] = (uint16_t)frame_len;
    }
    link->frame_have = 0;
}

void ble_controller_reset(void) {
    pthread_mutex_lock(&_lock);
    _out_head = _out_count = 0;
    _advertising = _scanning = _initiating = false;
    memset(_links, 0, sizeof(_links));
    memset(_peers, 0, sizeof(_peers));
    _phone = -1;
    _pdu_head = _pdu_count = 0;
    _le_mask = 0x1f;
    const char *fault = getenv("CIRCUITPY_BLE_VIRTUAL_FAULT");
    _no_public_address = fault != NULL && strcmp(fault, "no-public-address") == 0;
    _extended = fault != NULL && strcmp(fault, "extended") == 0;
    pthread_mutex_unlock(&_lock);
}

int ble_controller_available(void) {
    pthread_mutex_lock(&_lock);
    int n = (int)_out_count;
    pthread_mutex_unlock(&_lock);
    return n;
}

int ble_controller_read(uint8_t *data, size_t len) {
    pthread_mutex_lock(&_lock);
    size_t n = len < _out_count ? len : _out_count;
    for (size_t i = 0; i < n; i++) {
        data[i] = _out[_out_head];
        _out_head = (_out_head + 1) % sizeof(_out);
    }
    _out_count -= n;
    pthread_mutex_unlock(&_lock);
    return (int)n;
}

int ble_controller_write(const uint8_t *packet, size_t len) {
    pthread_mutex_lock(&_lock);
    if (packet[0] == H4_COMMAND && len >= 4) {
        _command((uint16_t)(packet[1] | packet[2] << 8), packet + 4, packet[3]);
    } else if (packet[0] == H4_ACL && len >= 5) {
        _acl(packet, len);
    }
    pthread_mutex_unlock(&_lock);
    return (int)len;
}

// ---- the phone ----

int ble_controller_connect(void) {
    pthread_mutex_lock(&_lock);
    int result = -1;
    if (_phone < 0) {
        int i = _peripheral_link(_phone_address, 0x01);
        if (i >= 0) {
            _phone = i;
            _pdu_head = _pdu_count = 0;
            result = 0;
        }
    }
    pthread_mutex_unlock(&_lock);
    return result;
}

void ble_controller_disconnect(uint8_t reason) {
    pthread_mutex_lock(&_lock);
    _link_close(_phone, reason, true);
    pthread_mutex_unlock(&_lock);
}

bool ble_controller_att_send(const uint8_t *pdu, size_t len) {
    pthread_mutex_lock(&_lock);
    bool ok = _phone >= 0 && len <= PDU_MAX;
    if (ok) {
        _to_host(_phone, L2CAP_CID_ATT, pdu, len);
    }
    pthread_mutex_unlock(&_lock);
    return ok;
}

size_t ble_controller_att_take(uint8_t *pdu, size_t max) {
    pthread_mutex_lock(&_lock);
    size_t len = 0;
    if (_pdu_count > 0) {
        len = _pdu_len[_pdu_head];
        if (len > max) {
            len = max;
        }
        memcpy(pdu, _pdus[_pdu_head], len);
        _pdu_head = (_pdu_head + 1) % PDU_COUNT;
        _pdu_count--;
    }
    pthread_mutex_unlock(&_lock);
    return len;
}

// ---- the air ----

int ble_controller_peer(const ble_peer_t *in) {
    pthread_mutex_lock(&_lock);
    peer_t *p = _peer(in->address);
    if (p == NULL) {
        for (int i = 0; i < PEER_COUNT && p == NULL; i++) {
            if (!_peers[i].used) {
                p = &_peers[i];
                memset(p, 0, sizeof(*p));
                p->used = true;
                memcpy(p->address, in->address, 6);
            }
        }
    }
    int result = p == NULL ? -1 : 0;
    if (p != NULL && in->gone) {
        _link_close(_link_to(p->address), ERR_REMOTE_USER, true);
        p->used = false;
    } else if (p != NULL && !in->advertising) {
        p->advertising = false;
    } else if (p != NULL) {
        bool changed = !p->advertising || p->data_len != in->data_len || p->scan_response_len != in->scan_response_len
            || memcmp(p->data, in->data, in->data_len) != 0
            || memcmp(p->scan_response, in->scan_response, in->scan_response_len) != 0
            || p->connectable != in->connectable;
        p->advertising = true;
        p->random = in->random;
        p->connectable = in->connectable;
        p->rssi = in->rssi;
        memcpy(p->data, in->data, in->data_len);
        p->data_len = in->data_len;
        memcpy(p->scan_response, in->scan_response, in->scan_response_len);
        p->scan_response_len = in->scan_response_len;
        if (changed) {
            p->reported = false;
        }
        _report(p);
        _try_initiate(p);
    }
    pthread_mutex_unlock(&_lock);
    return result;
}

int ble_controller_peer_connect(const uint8_t address[6], bool random) {
    pthread_mutex_lock(&_lock);
    int result = _peripheral_link(address, random ? 1 : 0) >= 0 ? 0 : -1;
    pthread_mutex_unlock(&_lock);
    return result;
}

int ble_controller_peer_disconnect(const uint8_t address[6], uint8_t reason) {
    pthread_mutex_lock(&_lock);
    int i = _link_to(address);
    _link_close(i, reason, true);
    pthread_mutex_unlock(&_lock);
    return i >= 0 ? 0 : -1;
}

int ble_controller_l2cap(const uint8_t address[6], uint16_t cid, const uint8_t *data, size_t len) {
    pthread_mutex_lock(&_lock);
    int i = len <= PDU_MAX ? _link_to(address) : -1;
    if (i >= 0) {
        _to_host(i, cid, data, len);
    }
    pthread_mutex_unlock(&_lock);
    return i >= 0 ? 0 : -1;
}

bool ble_controller_scanning(void) {
    pthread_mutex_lock(&_lock);
    bool on = _scanning;
    pthread_mutex_unlock(&_lock);
    return on;
}

size_t ble_controller_events(uint64_t since, int wait_ms, ble_event_t *out, size_t max, uint64_t *next, bool *lost) {
    pthread_mutex_lock(&_lock);
    *lost = false;
    uint64_t oldest = _event_end > EVENT_COUNT ? _event_end - EVENT_COUNT : 0;
    if (since > _event_end) {
        // A cursor of another run: from what there is.
        since = oldest;
        *lost = true;
        wait_ms = 0;
    }
    if (wait_ms > 0 && since >= _event_end) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += wait_ms / 1000;
        until.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        while (since >= _event_end && pthread_cond_timedwait(&_event_added, &_lock, &until) == 0) {
        }
    }
    oldest = _event_end > EVENT_COUNT ? _event_end - EVENT_COUNT : 0;
    if (since < oldest) {
        since = oldest;
        *lost = true;
    }
    size_t n = 0;
    for (uint64_t s = since; s < _event_end && n < max; s++) {
        out[n++] = _events[s % EVENT_COUNT];
    }
    *next = since + n;
    pthread_mutex_unlock(&_lock);
    return n;
}
