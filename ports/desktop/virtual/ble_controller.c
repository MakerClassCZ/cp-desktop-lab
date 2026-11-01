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

#include "common-hal/microcontroller/Processor.h"
#include "shared-bindings/microcontroller/Processor.h"
#include "shared-bindings/os/__init__.h"

#define H4_COMMAND 0x01
#define H4_ACL 0x02
#define H4_EVENT 0x04

#define EVT_COMMAND_COMPLETE 0x0e

#define ERR_UNKNOWN_COMMAND 0x01

static pthread_mutex_t _lock = PTHREAD_MUTEX_INITIALIZER;

// The bytes on their way to the host.
static uint8_t _out[16384];
static size_t _out_head;
static size_t _out_count;

// The command or ACL packet arriving from the host.
static uint8_t _in[1100];
static size_t _in_have;

static uint8_t _public_address[6];
static uint8_t _static_address[6];
static uint8_t _random_address[6];

// What the program advertises.
static uint8_t _adv_type;
static uint8_t _adv_data[31];
static uint8_t _adv_len;
static uint8_t _scan_rsp[31];
static uint8_t _scan_rsp_len;
static bool _advertising;

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

static void _ok(uint16_t opcode) {
    uint8_t status = 0;
    _complete(opcode, &status, 1);
}

static void _command(uint16_t opcode, const uint8_t *p, uint8_t len) {
    uint8_t ret[32] = { 0 };
    switch (opcode) {
        case 0x0c03:            // Reset
            _advertising = false;
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
            if (len >= 5) {
                _adv_type = p[4];
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

// A whole command or ACL packet from the host.
static void _from_host(const uint8_t *p, size_t len) {
    if (p[0] == H4_COMMAND) {
        _command((uint16_t)(p[1] | p[2] << 8), p + 4, p[3]);
    }
    (void)len;
}

void ble_controller_reset(void) {
    pthread_mutex_lock(&_lock);
    _out_head = _out_count = 0;
    _in_have = 0;
    _advertising = false;
    _adv_len = _scan_rsp_len = 0;
    // Addresses of this lab, from its UID: the public one and the static one in the "FICR".
    uint8_t uid[COMMON_HAL_MCU_PROCESSOR_UID_LENGTH];
    common_hal_mcu_processor_get_uid(uid);
    memcpy(_public_address, uid, 6);
    memcpy(_static_address, uid + 6, 6);
    _static_address[5] |= 0xc0;
    // A test reaches the host's path for a controller without one, as Zephyr's on an nRF is.
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
