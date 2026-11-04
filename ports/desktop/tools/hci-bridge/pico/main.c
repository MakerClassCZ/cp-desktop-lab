// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// A Pico 2 W (or a Pico W) as a Bluetooth controller on USB, for the desktop port's --ble PORT:
// the H4 packets that come on its USB serial port go to the CYW43439's Bluetooth core, and the
// core's events and data go back the same way, unchanged. The core is reached through the
// raspberrypi port's cyw43_hci.c, the same code its _bleio uses.

#include "pico/cyw43_arch.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include "cyw43_hci.h"

#define H4_COMMAND 0x01
#define H4_ACL 0x02
#define PACKET_MAX 1024

// The packet coming from the host, its type byte first (0 while a type byte is awaited), and how
// many bytes after the type are in.
static uint8_t _packet[1 + PACKET_MAX];
static size_t _have;

// A packet's whole length after its type byte; 0 while its header is not all there.
static size_t _length(void) {
    const uint8_t *p = _packet + 1;
    if (_packet[0] == H4_COMMAND) {
        return _have >= 3 ? 3 + (size_t)p[2] : 0;
    }
    return _have >= 4 ? 4 + (size_t)(p[2] | p[3] << 8) : 0;
}

static void _from_host(uint8_t byte) {
    if (_packet[0] == 0) {
        // Anything but a command or ACL data is not H4 from a host, and is dropped.
        if (byte == H4_COMMAND || byte == H4_ACL) {
            _packet[0] = byte;
            _have = 0;
        }
        return;
    }
    _packet[1 + _have++] = byte;
    size_t len = _length();
    if (len > PACKET_MAX) {
        _packet[0] = 0;
    } else if (len != 0 && _have == len) {
        cyw43_hci_send(_packet, 1 + len);
        _packet[0] = 0;
    }
}

int main(void) {
    stdio_usb_init();
    // The serial port carries packets only: nothing the SDK prints goes to it.
    stdio_set_driver_enabled(&stdio_usb, false);
    if (cyw43_arch_init() != 0 || cyw43_hci_init() != 0) {
        for (;;) {
            tight_loop_contents();
        }
    }
    // Lit: the core is up.
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    char buf[64];
    static uint8_t in[1 + PACKET_MAX];
    for (;;) {
        int n = stdio_usb.in_chars(buf, sizeof(buf));
        for (int i = 0; i < n; i++) {
            _from_host((uint8_t)buf[i]);
        }
        // The driver's poll reads the core's packets into cyw43_hci.c's queue when it has some.
        cyw43_arch_poll();
        size_t len;
        while ((len = cyw43_hci_receive(in, sizeof(in))) != 0) {
            stdio_usb.out_chars((const char *)in, (int)len);
        }
    }
}
