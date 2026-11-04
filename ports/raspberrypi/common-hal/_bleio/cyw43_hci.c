// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimír Smitka
//
// SPDX-License-Identifier: MIT

#include "cyw43_hci.h"

#include <string.h>

#include "cyw43.h"
#include "pico/cyw43_arch.h"
#include "pico/btstack_cyw43.h"

// cyw43_bluetooth_hci_read and _write keep a 4-byte header in the buffer: 3 bytes of length the
// driver fills in, then the H4 packet type. The packet itself follows.
#define HEADER 4
// The longest packet either way: an LE ACL packet of the 251 bytes the controller takes, or an
// event, with room to spare.
#define PACKET_MAX 1024
// Received packets wait here, each after its 2-byte length, until the host takes them. The
// controller sends as it receives from the peer, with no flow control towards the host. A 10-minute
// NUS echo with Wi-Fi on filled at most 136 bytes; 2 KB keeps about 7 full packets for a VM that
// stalls, as it does while writing flash.
#define QUEUE_SIZE 2048

static uint8_t _out[HEADER + PACKET_MAX] __attribute__((aligned(4)));
static uint8_t _in[HEADER + PACKET_MAX] __attribute__((aligned(4)));
static uint8_t _queue[QUEUE_SIZE];
// _head moves in the poll (cyw43_bluetooth_hci_process), _tail on the host's side.
static volatile size_t _head, _tail;
static volatile bool _queueing;
uint32_t cyw43_hci_dropped;

static void _queue_put(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        _queue[(_head + i) % QUEUE_SIZE] = data[i];
    }
    _head += len;
}

static void _queue_get(uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        data[i] = _queue[(_tail + i) % QUEUE_SIZE];
    }
    _tail += len;
}

// The driver calls this from its poll, the lock held, when the Bluetooth core has work. Every
// waiting packet is read here: until they are all read the core keeps its interrupt up, and the
// poll would run again at once, for ever, with nothing else getting a turn.
void cyw43_bluetooth_hci_process(void) {
    for (;;) {
        uint32_t len = 0;
        if (cyw43_bluetooth_hci_read(_in, sizeof(_in), &len) != 0 || len <= HEADER) {
            return;
        }
        const size_t h4_len = len - (HEADER - 1);   // the type byte, then the packet
        if (!_queueing) {
            continue;
        }
        if (2 + h4_len > QUEUE_SIZE - (_head - _tail)) {
            cyw43_hci_dropped++;
            continue;
        }
        const uint8_t size[2] = { h4_len & 0xff, h4_len >> 8 };
        _queue_put(size, 2);
        _queue_put(_in + HEADER - 1, h4_len);
    }
}

int cyw43_hci_init(void) {
    cyw43_thread_enter();
    _head = _tail = 0;
    int result = cyw43_bluetooth_hci_init();
    _queueing = result == 0;
    cyw43_thread_exit();
    return result;
}

void cyw43_hci_deinit(void) {
    cyw43_thread_enter();
    _queueing = false;
    _head = _tail = 0;
    cyw43_thread_exit();
}

bool cyw43_hci_send(const uint8_t *h4, size_t len) {
    if (len == 0 || len > PACKET_MAX + 1) {
        return false;
    }
    cyw43_thread_enter();
    memcpy(_out + HEADER - 1, h4, len);
    int result = cyw43_bluetooth_hci_write(_out, HEADER - 1 + len);
    cyw43_thread_exit();
    return result == 0;
}

size_t cyw43_hci_receive(uint8_t *h4, size_t max) {
    if (_head - _tail < 2) {
        return 0;
    }
    uint8_t size[2];
    _queue_get(size, 2);
    const size_t len = size[0] | size[1] << 8;
    if (len > max) {
        _tail += len;
        cyw43_hci_dropped++;
        return 0;
    }
    _queue_get(h4, len);
    return len;
}

// cyw43_arch calls these when CYW43_ENABLE_BLUETOOTH is set, to start BTstack. The host here is
// another (the caller of cyw43_hci_*), so they do nothing.
bool btstack_cyw43_init(async_context_t *context) {
    (void)context;
    return true;
}

void btstack_cyw43_deinit(async_context_t *context) {
    (void)context;
}
