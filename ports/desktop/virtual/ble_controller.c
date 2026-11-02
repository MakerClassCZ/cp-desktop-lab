// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// A Bluetooth LE controller in the lab, for _bleio's HCI host stack: it answers the commands the
// host sends as a real controller does (the values a Pico 2 W's CYW43439 gave), keeps what the
// program advertises, and joins one peer to it, the phone, which the control API plays. It has no
// time of its own: nothing happens until the host or the phone does something.

#include "ble_controller.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "common-hal/microcontroller/Processor.h"
#include "shared-bindings/microcontroller/Processor.h"
#include "shared-bindings/os/__init__.h"

#define H4_COMMAND 0x01
#define H4_ACL 0x02
#define H4_EVENT 0x04

#define EVT_DISCONNECTION_COMPLETE 0x05
#define EVT_COMMAND_COMPLETE 0x0e
#define EVT_COMMAND_STATUS 0x0f
#define EVT_NUMBER_OF_COMPLETED_PACKETS 0x13
#define EVT_LE_META 0x3e
#define EVT_VENDOR 0xff

#define L2CAP_CID_ATT 0x0004
#define HANDLE 0x0040          // the one connection's, as the CYW43439 numbers its first

#define ERR_UNKNOWN_COMMAND 0x01

static pthread_mutex_t _lock = PTHREAD_MUTEX_INITIALIZER;

// The bytes on their way to the host.
static uint8_t _out[16384];
static size_t _out_head;
static size_t _out_count;

// The command or ACL packet arriving from the host.
static uint8_t _in[1100];
static size_t _in_have;

// A test's fault (CIRCUITPY_BLE_VIRTUAL_FAULT): "no-completed" keeps the host's ACL credit,
// "silent" answers no command, as a controller that hangs.
static enum { FAULT_NONE, FAULT_NO_COMPLETED, FAULT_SILENT } _fault;

static uint8_t _public_address[6];
static uint8_t _static_address[6];
static uint8_t _random_address[6];

// What the program advertises.
static uint8_t _adv_type;
static uint8_t _adv_own_address_type;
static uint8_t _adv_data[31];
static uint8_t _adv_len;
static uint8_t _scan_rsp[31];
static uint8_t _scan_rsp_len;
static bool _advertising;

// The phone: a random static address, as a phone's is; the connection to it.
static const uint8_t _phone_address[6] = { 0x89, 0x37, 0x1c, 0xc9, 0x10, 0x7c };
static bool _connected;

// ATT PDUs the program sent the phone, oldest first; the L2CAP frame being reassembled.
#define PDU_MAX 512
#define PDU_COUNT 256
static uint8_t _pdus[PDU_COUNT][PDU_MAX];
static uint16_t _pdu_len[PDU_COUNT];
static size_t _pdu_head;
static size_t _pdu_count;
static uint8_t _frame[PDU_MAX + 4];
static size_t _frame_have;

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

static void _disconnected(uint8_t reason) {
    if (!_connected) {
        return;
    }
    _connected = false;
    _frame_have = 0;
    uint8_t params[4] = { 0, HANDLE & 0xff, HANDLE >> 8, reason };
    _event(EVT_DISCONNECTION_COMPLETE, params, sizeof(params));
}

static void _ok(uint16_t opcode) {
    uint8_t status = 0;
    _complete(opcode, &status, 1);
}

static void _command(uint16_t opcode, const uint8_t *p, uint8_t len) {
    uint8_t ret[32] = { 0 };
    switch (opcode) {
        case 0x0c03:            // Reset
            _advertising = false;
            _connected = false;
            _adv_len = _scan_rsp_len = 0;
            _ok(opcode);
            break;
        case 0x0c01:            // Set Event Mask
        case 0x2001:            // LE Set Event Mask
        case 0x200b:            // LE Set Scan Parameters
        case 0x200c:            // LE Set Scan Enable
            _ok(opcode);
            break;
        case 0x1001:            // Read Local Version Information: Bluetooth 5.2, no manufacturer
            ret[1] = 0x0b;
            ret[4] = 0x0b;
            ret[5] = 0xff;
            ret[6] = 0xff;
            _complete(opcode, ret, 9);
            break;
        case 0x1009:            // Read BD_ADDR
            memcpy(ret + 1, _public_address, 6);
            _complete(opcode, ret, 7);
            break;
        case 0x2002:            // LE Read Buffer Size: 251 bytes, 8 packets
            ret[1] = 251;
            ret[3] = 8;
            _complete(opcode, ret, 4);
            break;
        case 0x2003:            // LE Read Local Supported Features: encryption only, no extended advertising
            ret[1] = 0x01;
            _complete(opcode, ret, 9);
            break;
        case 0x2005:            // LE Set Random Address
            if (len >= 6) {
                memcpy(_random_address, p, 6);
            }
            _ok(opcode);
            break;
        case 0x2006:            // LE Set Advertising Parameters
            if (len >= 6) {
                _adv_type = p[4];
                _adv_own_address_type = p[5];
            }
            _ok(opcode);
            break;
        case 0x2008:            // LE Set Advertising Data
        case 0x2009:            // LE Set Scan Response Data
            if (len >= 1) {
                uint8_t n = p[0] <= 31 && p[0] < len ? p[0] : 0;
                memcpy(opcode == 0x2008 ? _adv_data : _scan_rsp, p + 1, n);
                *(opcode == 0x2008 ? &_adv_len : &_scan_rsp_len) = n;
            }
            _ok(opcode);
            break;
        case 0x200a:            // LE Set Advertising Enable
            _advertising = len >= 1 && p[0] != 0;
            _ok(opcode);
            break;
        case 0x0406:            // Disconnect: the host ends the connection
            _status(opcode, _connected ? 0 : 0x02);
            _disconnected(0x16);
            break;
        case 0x2018:            // LE Rand
            common_hal_os_urandom(ret + 1, 8);
            _complete(opcode, ret, 9);
            break;
        case 0xfc09:            // Zephyr's Read Static Addresses: one, with a zero identity root
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

// ACL data from the host to the phone: an L2CAP frame, maybe in fragments; an ATT one goes
// to the phone's queue. Each packet is acknowledged, so the host's credit comes back.
static void _acl(const uint8_t *p, size_t len) {
    uint16_t flags = p[2] >> 4;
    size_t n = len - 5;
    if ((flags & 0x3) != 0x1) {
        _frame_have = 0;   // the first fragment of a frame
    }
    if (_frame_have + n <= sizeof(_frame)) {
        memcpy(_frame + _frame_have, p + 5, n);
        _frame_have += n;
    }
    if (_fault != FAULT_NO_COMPLETED) {
        uint8_t done[5] = { 1, HANDLE & 0xff, HANDLE >> 8, 1, 0 };
        _event(EVT_NUMBER_OF_COMPLETED_PACKETS, done, sizeof(done));
    }
    if (_frame_have < 4) {
        return;
    }
    size_t frame_len = (size_t)(_frame[0] | _frame[1] << 8);
    uint16_t cid = (uint16_t)(_frame[2] | _frame[3] << 8);
    if (_frame_have < 4 + frame_len) {
        return;
    }
    if (_connected && cid == L2CAP_CID_ATT && frame_len <= PDU_MAX && _pdu_count < PDU_COUNT) {
        size_t slot = (_pdu_head + _pdu_count++) % PDU_COUNT;
        memcpy(_pdus[slot], _frame + 4, frame_len);
        _pdu_len[slot] = (uint16_t)frame_len;
    }
    _frame_have = 0;
}

// A whole command or ACL packet from the host.
static void _from_host(const uint8_t *p, size_t len) {
    if (p[0] == H4_COMMAND && _fault != FAULT_SILENT) {
        _command((uint16_t)(p[1] | p[2] << 8), p + 4, p[3]);
    } else if (p[0] == H4_ACL && len >= 5) {
        _acl(p, len);
    }
}

// No machine id (Windows, macOS): sixteen bytes from the computer's name, the same at each start.
static void _uid_from_name(uint8_t uid[COMMON_HAL_MCU_PROCESSOR_UID_LENGTH]) {
    char name[256] = "";
    const char *env = getenv("COMPUTERNAME");
    if (env != NULL) {
        strncpy(name, env, sizeof(name) - 1);
    }
    #ifndef _WIN32
    if (name[0] == '\0') {
        gethostname(name, sizeof(name) - 1);
    }
    #endif
    // FNV-1a over the name, once for each byte.
    uint32_t h = 2166136261u;
    for (int i = 0; i < COMMON_HAL_MCU_PROCESSOR_UID_LENGTH; i++) {
        for (const char *c = name; *c != '\0'; c++) {
            h = (h ^ (uint8_t)*c) * 16777619u;
        }
        h = (h ^ (uint32_t)i) * 16777619u;
        uid[i] = (uint8_t)(h >> 24);
    }
}

void ble_controller_reset(void) {
    pthread_mutex_lock(&_lock);
    _out_head = _out_count = 0;
    _in_have = 0;
    _advertising = false;
    _adv_len = _scan_rsp_len = 0;
    _connected = false;
    _pdu_head = _pdu_count = 0;
    _frame_have = 0;
    // Addresses of this lab, from its UID: the public one and the static one in the "FICR".
    uint8_t uid[COMMON_HAL_MCU_PROCESSOR_UID_LENGTH];
    common_hal_mcu_processor_get_uid(uid);
    static const uint8_t no_uid[COMMON_HAL_MCU_PROCESSOR_UID_LENGTH] = { 0 };
    if (memcmp(uid, no_uid, sizeof(no_uid)) == 0) {
        _uid_from_name(uid);
    }
    memcpy(_public_address, uid, 6);
    memcpy(_static_address, uid + 6, 6);
    _static_address[5] |= 0xc0;
    // A test reaches the host's path for a controller without one, as Zephyr's on an nRF is.
    const char *fault = getenv("CIRCUITPY_BLE_VIRTUAL_FAULT");
    _fault = fault == NULL ? FAULT_NONE : strcmp(fault, "no-completed") == 0 ? FAULT_NO_COMPLETED
        : strcmp(fault, "silent") == 0 ? FAULT_SILENT : FAULT_NONE;
    const char *no_public = getenv("CIRCUITPY_BLE_NO_PUBLIC_ADDRESS");
    if (no_public != NULL && no_public[0] == '1') {
        memset(_public_address, 0, sizeof(_public_address));
    }
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

int ble_controller_write(const uint8_t *data, size_t len) {
    pthread_mutex_lock(&_lock);
    for (size_t i = 0; i < len; i++) {
        _in[_in_have++] = data[i];
        size_t whole = 0;
        if (_in[0] == H4_COMMAND && _in_have >= 4) {
            whole = 4 + _in[3];
        } else if (_in[0] == H4_ACL && _in_have >= 5) {
            whole = 5 + (size_t)(_in[3] | _in[4] << 8);
        } else if (_in[0] != H4_COMMAND && _in[0] != H4_ACL) {
            _in_have = 0;
            continue;
        }
        if (whole > sizeof(_in)) {
            _in_have = 0;
        } else if (whole != 0 && _in_have == whole) {
            _from_host(_in, whole);
            _in_have = 0;
        }
    }
    pthread_mutex_unlock(&_lock);
    return (int)len;
}

int ble_controller_connect(void) {
    pthread_mutex_lock(&_lock);
    int result = -1;
    if (_advertising && (_adv_type == 0x00 || _adv_type == 0x01) && !_connected) {
        _advertising = false;
        _connected = true;
        _pdu_head = _pdu_count = 0;
        // LE Connection Complete: the lab is the peripheral, the phone the central; a 30 ms
        // interval, no latency, a 2 s supervision timeout.
        uint8_t params[19] = { 0x01, 0x00, HANDLE & 0xff, HANDLE >> 8, 0x01, 0x01 };
        memcpy(params + 6, _phone_address, 6);
        params[12] = 0x18;
        params[16] = 0xc8;
        _event(EVT_LE_META, params, sizeof(params));
        // A vendor event, as the CYW43439 sends right after a connection: a host must let it pass.
        uint8_t vendor[5] = { 0x55, 0x00, 0x00, HANDLE & 0xff, HANDLE >> 8 };
        _event(EVT_VENDOR, vendor, sizeof(vendor));
        result = 0;
    }
    pthread_mutex_unlock(&_lock);
    return result;
}

void ble_controller_disconnect(uint8_t reason) {
    pthread_mutex_lock(&_lock);
    _disconnected(reason);
    pthread_mutex_unlock(&_lock);
}

bool ble_controller_att_send(const uint8_t *pdu, size_t len) {
    pthread_mutex_lock(&_lock);
    bool ok = _connected && len <= PDU_MAX;
    if (ok) {
        // An L2CAP frame on the ATT channel, in ACL packets of at most 251 bytes, as LE's are:
        // the first flagged as a start, the rest as continuations.
        uint8_t frame[4 + PDU_MAX] = { (uint8_t)len, (uint8_t)(len >> 8), L2CAP_CID_ATT & 0xff, L2CAP_CID_ATT >> 8 };
        memcpy(frame + 4, pdu, len);
        for (size_t at = 0; at < len + 4; at += 251) {
            size_t n = len + 4 - at < 251 ? len + 4 - at : 251;
            uint8_t head[5] = { H4_ACL, HANDLE & 0xff, (HANDLE >> 8) | (at == 0 ? 0x20 : 0x10), (uint8_t)n, (uint8_t)(n >> 8) };
            _put(head, sizeof(head));
            _put(frame + at, n);
        }
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
