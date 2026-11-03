// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimír Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The link between the HCI host in hci.c and its controller. The UART one (an AirLift, wired up by
// the program as _bleio.Adapter(uart=...)) is below; a port whose board has its controller built
// in gives _bleio its own. Every callback runs on the VM's thread.
typedef struct {
    // Bytes ready to read now.
    size_t (*available)(void *ctx);
    // Up to len bytes into data; never waits. 0 when there are none.
    size_t (*read)(void *ctx, uint8_t *data, size_t len);
    // One whole H4 packet (its type byte first). HCI_OK, HCI_WRITE_TIMEOUT when the controller
    // would not take it in time, or HCI_WRITE_ERROR when the link failed.
    int (*write)(void *ctx, const uint8_t *h4, size_t len);
    // The adapter turned on (before its HCI Reset) or off (after it). May be NULL.
    void (*enabled)(void *ctx, bool on);
    // false while hci.c handles a complete packet, true when it can take more: a link with flow
    // control holds the controller off in between. May be NULL.
    void (*ready)(void *ctx, bool ready);
} bleio_hci_transport_t;

// ctx is kept in the adapter, which is a GC root: a heap object there stays alive.
typedef struct _bleio_adapter_obj_t bleio_adapter_obj_t;
void common_hal_bleio_adapter_construct_hci_transport(bleio_adapter_obj_t *self,
    const bleio_hci_transport_t *transport, void *ctx);

// The UART link with optional RTS and CTS pins; its ctx is the adapter.
extern const bleio_hci_transport_t bleio_hci_uart_transport;
