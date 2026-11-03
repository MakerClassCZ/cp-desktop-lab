// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// Bluetooth LE for _bleio (devices/ble_hci, the AirLift boards' host stack): the HCI link behind
// the port's own adapter. The controller is the virtual one (virtual/ble_controller.c) or a real
// one on a serial port of the computer (--ble PORT: a Pico 2 W bridge, Zephyr's hci_uart), and
// every H4 packet goes to ble_host_packet() on its way, both ways.

#include "ble_host.h"

#include <stdio.h>
#include <string.h>

#include "common-hal/_bleio/Adapter.h"
#include "common-hal/_bleio/hci.h"
#include "common-hal/_bleio/hci_transport.h"
#include "shared-bindings/_bleio/__init__.h"

#include "ble_controller.h"
#include "desktop_port.h"
#include "serial_host.h"

static bool _board_off;         // a board file's [board] ble = "off"
static bool _on;                // a controller is there
static bool _real;              // ... on a serial port
static serial_host_t _serial;

// The controller's bytes gathered into whole H4 packets: the host reads them a byte at a time.
// What the host writes is a whole packet each time.
static uint8_t _packet[1100];
static size_t _have;

// An H4 packet's whole length from its first bytes; 0 while its header is not all there.
static size_t _h4_length(const uint8_t *p, size_t have) {
    switch (p[0]) {
        case 0x02:              // ACL data: handle and flags, 16-bit length
            return have >= 5 ? 5 + (size_t)(p[3] | p[4] << 8) : 0;
        case 0x04:              // event: code, length
            return have >= 3 ? 3 + (size_t)p[2] : 0;
        default:                // not H4 from a controller: dropped a byte at a time
            return 1;
    }
}

int ble_host_available(void) {
    return _real ? serial_host_available(&_serial) : ble_controller_available();
}

int ble_host_read(uint8_t *data, size_t len) {
    int n = _real ? serial_host_read(&_serial, data, len, 0) : ble_controller_read(data, len);
    for (int i = 0; i < n; i++) {
        _packet[_have++] = data[i];
        size_t whole = _h4_length(_packet, _have);
        if (whole > sizeof(_packet)) {
            _have = 0;
        } else if (whole != 0 && _have == whole) {
            if (_packet[0] == 0x02 || _packet[0] == 0x04) {
                ble_host_packet(false, _packet, whole);
            }
            _have = 0;
        }
    }
    return n;
}

int ble_host_write(const uint8_t *data, size_t len) {
    int n = _real ? serial_host_write(&_serial, data, len, 1000) : ble_controller_write(data, len);
    if (n > 0) {
        ble_host_packet(true, data, (size_t)n);
    }
    return n;
}

// The link as ble_hci's transport (hci_transport.h): what is there now, never a wait.
static size_t _transport_available(void *ctx) {
    (void)ctx;
    int n = ble_host_available();
    return n > 0 ? (size_t)n : 0;
}

static size_t _transport_read(void *ctx, uint8_t *data, size_t len) {
    (void)ctx;
    int n = ble_host_read(data, len);
    return n > 0 ? (size_t)n : 0;
}

static int _transport_write(void *ctx, const uint8_t *h4, size_t len) {
    (void)ctx;
    int n = ble_host_write(h4, len);
    return n < 0 ? HCI_WRITE_ERROR : (size_t)n < len ? HCI_WRITE_TIMEOUT : HCI_OK;
}

static const bleio_hci_transport_t _transport = {
    .available = _transport_available,
    .read = _transport_read,
    .write = _transport_write,
};

bool ble_host_board_setting(const char *value) {
    if (strcmp(value, "on") == 0 || strcmp(value, "off") == 0) {
        _board_off = strcmp(value, "off") == 0;
        return true;
    }
    fprintf(stderr, "circuitpython: [board] ble %s: expected on or off\n", value);
    return false;
}

const char *ble_host_port(void) {
    const char *ble = desktop_options.ble;
    return ble == NULL || strcmp(ble, "off") == 0 || strcmp(ble, "virtual") == 0 ? NULL : ble;
}

bool ble_host_init(void) {
    const char *ble = desktop_options.ble;
    if (ble == NULL ? _board_off : strcmp(ble, "off") == 0) {
        return true;
    }
    const char *port = ble_host_port();
    if (port == NULL) {
        ble_controller_reset();
    } else {
        char err[256];
        if (!serial_host_open(&_serial, port, err, sizeof(err))) {
            fprintf(stderr, "circuitpython: --ble: %s\n", err);
            return false;
        }
        // The baud rate means nothing to a USB CDC port; a UART adapter would want 115200.
        serial_host_configure(&_serial, 115200, 8, 'N', 1);
        serial_host_flush_input(&_serial);
        _real = true;
    }
    if (desktop_options.ble_log != NULL && !ble_host_open_log(desktop_options.ble_log)) {
        return false;
    }
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
    adapter->hci_transport = &_transport;
    adapter->hci_transport_ctx = NULL;
    adapter->enabled = false;
    bleio_set_adapter(MP_OBJ_FROM_PTR(adapter));
}

const char *ble_host_controller(void) {
    return !_on ? "off" : _real ? "serial" : "virtual";
}
