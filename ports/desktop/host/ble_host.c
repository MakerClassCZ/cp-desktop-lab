// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// Bluetooth LE for _bleio (devices/ble_hci, the AirLift boards' host stack): the HCI link behind
// the port's own adapter. The controller is the virtual one (virtual/ble_controller.c) or a real
// one on a serial port of the computer (--ble PORT: a Pico 2 W bridge, Zephyr's hci_uart), and
// every H4 packet is framed here on its way, both ways.

#include "ble_host.h"

#include <stdio.h>
#include <string.h>

#include "common-hal/_bleio/Adapter.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/busio/UART.h"

#include "ble_controller.h"
#include "desktop_port.h"
#include "serial_host.h"

static bool _board_off;         // a board file's [board] ble = "off"
static bool _on;                // a controller is there
static bool _real;              // ... on a serial port
static serial_host_t _serial;
static busio_uart_obj_t _uart;  // the adapter's hci_uart

// One direction's bytes gathered into whole H4 packets.
typedef struct {
    uint8_t data[1100];
    size_t have;
} framer_t;

static framer_t _from_controller;
static framer_t _to_controller;

// An H4 packet's whole length from its first bytes; 0 while its header is not all there.
static size_t _h4_length(const uint8_t *p, size_t have) {
    switch (p[0]) {
        case 0x01:              // command: opcode, length
            return have >= 4 ? 4 + (size_t)p[3] : 0;
        case 0x02:              // ACL data: handle and flags, 16-bit length
            return have >= 5 ? 5 + (size_t)(p[3] | p[4] << 8) : 0;
        case 0x04:              // event: code, length
            return have >= 3 ? 3 + (size_t)p[2] : 0;
        default:                // not H4: dropped a byte at a time
            return 1;
    }
}

static void _frame(framer_t *f, bool to_controller, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        f->data[f->have++] = data[i];
        size_t whole = _h4_length(f->data, f->have);
        if (whole > sizeof(f->data)) {
            f->have = 0;
        } else if (whole != 0 && f->have == whole) {
            if (f->data[0] == 0x01 || f->data[0] == 0x02 || f->data[0] == 0x04) {
                ble_host_packet(to_controller, f->data, whole);
            }
            f->have = 0;
        }
    }
}

static int _available(void) {
    return _real ? serial_host_available(&_serial) : ble_controller_available();
}

static int _read(uint8_t *data, size_t len) {
    int n = _real ? serial_host_read(&_serial, data, len, 0) : ble_controller_read(data, len);
    if (n > 0) {
        _frame(&_from_controller, false, data, (size_t)n);
    }
    return n;
}

static int _write(const uint8_t *data, size_t len) {
    int n = _real ? serial_host_write(&_serial, data, len, 1000) : ble_controller_write(data, len);
    if (n > 0) {
        _frame(&_to_controller, true, data, (size_t)n);
    }
    return n;
}

static const ble_transport_t _transport = { _available, _read, _write };

bool ble_host_board_setting(const char *value) {
    if (strcmp(value, "on") == 0 || strcmp(value, "off") == 0) {
        _board_off = strcmp(value, "off") == 0;
        return true;
    }
    fprintf(stderr, "circuitpython: [board] ble %s: expected on or off\n", value);
    return false;
}

bool ble_host_init(void) {
    const char *ble = desktop_options.ble;
    if (ble == NULL ? _board_off : strcmp(ble, "off") == 0) {
        return true;
    }
    if (ble == NULL || strcmp(ble, "virtual") == 0) {
        ble_controller_reset();
    } else {
        char err[256];
        if (!serial_host_open(&_serial, ble, err, sizeof(err))) {
            fprintf(stderr, "circuitpython: --ble: %s\n", err);
            return false;
        }
        // The baud rate means nothing to a USB CDC port; a UART adapter would want 115200.
        serial_host_configure(&_serial, 115200, 8, 'N', 1);
        serial_host_flush_input(&_serial);
        _real = true;
    }
    _uart.base.type = &busio_uart_type;
    _uart.open = true;
    _uart.ble = &_transport;
    _on = true;
    return true;
}

void ble_host_attach_adapter(void) {
    if (!_on) {
        return;
    }
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    adapter->allocated = true;
    adapter->persistent = true;
    adapter->hci_uart = &_uart;
    adapter->rts_digitalinout = NULL;
    adapter->cts_digitalinout = NULL;
    adapter->enabled = false;
    bleio_set_adapter(MP_OBJ_FROM_PTR(adapter));
}

void ble_host_packet(bool to_controller, const uint8_t *h4, size_t len) {
    (void)to_controller;
    (void)h4;
    (void)len;
}
