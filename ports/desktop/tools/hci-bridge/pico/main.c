// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// A Pico 2 W (or a Pico W) as a Bluetooth controller on USB, for the desktop port's --ble PORT:
// the H4 packets that come on its USB serial port go to the CYW43439's Bluetooth core, and the
// core's events and data go back the same way, unchanged.

#include "pico/cyw43_arch.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#define H4_COMMAND 0x01
#define H4_ACL 0x02

// cyw43_bluetooth_hci_read and _write keep a 4-byte header before the packet, the packet's H4
// type in its last byte.
#define HEADER 4
#define PACKET_MAX 1024

static uint8_t _out[HEADER + PACKET_MAX] __attribute__((aligned(4)));
static uint8_t _in[HEADER + PACKET_MAX] __attribute__((aligned(4)));

// The packet coming from the host: its type (0 while a type byte is awaited), the bytes after it.
static uint8_t _type;
static size_t _have;

// The driver's poll calls this when the core has packets. Reading only then matters: each read
// wakes the core and interrupts it, and a core read without a pause crashes.
static volatile bool _bt_work;

void cyw43_bluetooth_hci_process(void) {
    _bt_work = true;
}

// The SDK's hooks for BTstack, which the bridge does without: the host is at the end of the USB.
bool btstack_cyw43_init(async_context_t *context) {
    (void)context;
    return true;
}

void btstack_cyw43_deinit(async_context_t *context) {
    (void)context;
}

// A packet's whole length after its type byte; 0 while its header is not all there.
static size_t _length(const uint8_t *p) {
    if (_type == H4_COMMAND) {
        return _have >= 3 ? 3 + (size_t)p[2] : 0;
    }
    return _have >= 4 ? 4 + (size_t)(p[2] | p[3] << 8) : 0;
}

static void _from_host(uint8_t byte) {
    if (_type == 0) {
        // Anything but a command or ACL data is not H4 from a host, and is dropped.
        if (byte == H4_COMMAND || byte == H4_ACL) {
            _type = byte;
            _have = 0;
        }
        return;
    }
    uint8_t *p = _out + HEADER;
    p[_have++] = byte;
    size_t len = _length(p);
    if (len > PACKET_MAX) {
        _type = 0;
    } else if (len != 0 && _have == len) {
        _out[3] = _type;
        cyw43_bluetooth_hci_write(_out, HEADER + len);
        _type = 0;
    }
}

static void _to_host(void) {
    for (;;) {
        uint32_t len = 0;
        if (cyw43_bluetooth_hci_read(_in, sizeof(_in), &len) != 0 || len <= HEADER) {
            return;
        }
        // The type byte, then the packet: they start at the header's last byte.
        stdio_usb.out_chars((const char *)_in + 3, (int)(len - 3));
    }
}

int main(void) {
    stdio_usb_init();
    // The serial port carries packets only: nothing the SDK prints goes to it.
    stdio_set_driver_enabled(&stdio_usb, false);
    if (cyw43_arch_init() != 0 || cyw43_bluetooth_hci_init() != 0) {
        for (;;) {
            tight_loop_contents();
        }
    }
    // Lit: the core is up.
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    char buf[64];
    for (;;) {
        int n = stdio_usb.in_chars(buf, sizeof(buf));
        for (int i = 0; i < n; i++) {
            _from_host((uint8_t)buf[i]);
        }
        cyw43_arch_poll();
        if (_bt_work) {
            _bt_work = false;
            _to_host();
        }
    }
}
