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
#include <stdlib.h>
#include <string.h>

#include "common-hal/_bleio/Adapter.h"
#include "common-hal/_bleio/bonding.h"
#include "common-hal/_bleio/hci.h"
#include "common-hal/_bleio/hci_transport.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Characteristic.h"
#include "shared-bindings/_bleio/Descriptor.h"
#include "shared-bindings/microcontroller/__init__.h"
#include "supervisor/port.h"
#include "supervisor/shared/bluetooth/bluetooth.h"

#include "ble_controller.h"
#include "desktop_port.h"
#include "platform.h"
#include "serial_host.h"

static bool _board_off;         // a board file's [board] ble = "off"
static const char *_board_workflow;  // a board file's [board] ble_workflow
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

bool ble_host_board_workflow(const char *value) {
    if (strcmp(value, "on") == 0 || strcmp(value, "discover") == 0 || strcmp(value, "off") == 0) {
        _board_workflow = value;
        return true;
    }
    fprintf(stderr, "circuitpython: [board] ble_workflow %s: expected on, discover or off\n", value);
    return false;
}

const char *ble_host_workflow_mode(void) {
    return desktop_options.ble_workflow != NULL ? desktop_options.ble_workflow
           : _board_workflow != NULL ? _board_workflow : "off";
}

bool ble_host_init(void) {
    const char *ble = desktop_options.ble;
    const char *workflow = ble_host_workflow_mode();
    const bool off = ble == NULL ? _board_off : strcmp(ble, "off") == 0;
    if (strcmp(workflow, "on") != 0 && strcmp(workflow, "discover") != 0 && strcmp(workflow, "off") != 0) {
        fprintf(stderr, "circuitpython: --ble-workflow %s: expected on, discover or off\n", workflow);
        return false;
    }
    const bool workflow_on = strcmp(workflow, "off") != 0;
    if (workflow_on && off) {
        fprintf(stderr, "circuitpython: --ble-workflow %s needs Bluetooth, and --ble (or the board file) turns it off\n", workflow);
        return false;
    }
    if (off) {
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

// The supervisor's discovery word (supervisor/shared/bluetooth/bluetooth.c): the next boot erases
// the bonds and advertises publicly, as after reset with the boot button held.
#define DISCOVERY_WORD (0xbb0000bb | 1 << 8)

void ble_host_port_init(void) {
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
    // discover: once per launch; a reset re-runs the process with the same arguments and boots as on.
    if (strcmp(ble_host_workflow_mode(), "discover") == 0 && !desktop_was_reset) {
        port_set_saved_word(DISCOVERY_WORD);
    }
}

void ble_host_board_init(void) {
    if (!_on || strcmp(ble_host_workflow_mode(), "off") == 0) {
        supervisor_bluetooth_disable_workflow();
    }
}

static volatile bool _discovery_requested;

bool ble_host_request_discovery(void) {
    if (!_on || strcmp(ble_host_workflow_mode(), "off") == 0) {
        return false;
    }
    __atomic_store_n(&_discovery_requested, true, __ATOMIC_RELEASE);
    return true;
}

void ble_host_vm_task(void) {
    if (__atomic_load_n(&_discovery_requested, __ATOMIC_ACQUIRE)) {
        _discovery_requested = false;
        port_set_saved_word(DISCOVERY_WORD);
        common_hal_mcu_reset();
    }
}

bool ble_host_bonded(void) {
    return _on && common_hal_bleio_adapter_is_bonded_to_central(&common_hal_bleio_adapter_obj);
}

const char *ble_host_workflow_state(void) {
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    if (!_on || !supervisor_bluetooth_workflow_is_enabled()) {
        return "off";
    }
    ble_host_state_t s;
    ble_host_state(&s);
    if (s.connected && !adapter->now_advertising) {
        return "connected";
    }
    if (s.advertising && adapter->circuitpython_advertising) {
        // Discovery mode's advertisement has the name in a scan response; the private one has none.
        return s.scan_response_len > 0 ? "discovery" : "reconnecting";
    }
    return "on";
}

const char *ble_host_controller(void) {
    return !_on ? "off" : _real ? "serial" : "virtual";
}

// The bond store's blob (devices/ble_hci bonding.c) in a file beside the drive, CIRCUITPY.bonds
// (--ble-bonds): the same bytes a board keeps in flash, with the bonds of every controller the lab
// used, each under its identity. Read once a run, written whole at each change.
size_t bleio_hci_bonding_storage_read(uint8_t *buf, size_t len) {
    FILE *f = fopen(desktop_options.ble_bonds_path, "rb");
    if (f == NULL) {
        return 0;
    }
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    ble_host_bonds_stored(buf, n, false);
    return n;
}

bool bleio_hci_bonding_storage_write(const uint8_t *buf, size_t len) {
    FILE *f = fopen(desktop_options.ble_bonds_path, "wb");
    bool ok = f != NULL && fwrite(buf, 1, len, f) == len;
    if (f != NULL && fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "circuitpython: the bonds were not saved in %s\n", desktop_options.ble_bonds_path);
    }
    ble_host_bonds_stored(buf, len, true);
    return ok;
}
