// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2020 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimír Smitka
//
// SPDX-License-Identifier: MIT

// The HCI link over a UART, as to an AirLift: RTS and CTS are optional pins (NULL when the link
// has its own flow control).

#include "py/mphal.h"
#include "py/runtime.h"
#include "supervisor/shared/tick.h"
#include "shared-bindings/busio/UART.h"
#include "shared-bindings/digitalio/DigitalInOut.h"

#include "common-hal/_bleio/Adapter.h"
#include "common-hal/_bleio/hci.h"
#include "common-hal/_bleio/hci_transport.h"

#define CTS_TIMEOUT_MSECS (1000)

static size_t uart_available(void *ctx) {
    bleio_adapter_obj_t *adapter = ctx;
    return common_hal_busio_uart_rx_characters_available(adapter->hci_uart);
}

static size_t uart_read(void *ctx, uint8_t *data, size_t len) {
    bleio_adapter_obj_t *adapter = ctx;
    int errcode = 0;
    size_t n = common_hal_busio_uart_read(adapter->hci_uart, data, len, &errcode);
    // Nothing there yet (EAGAIN) or a failed read: nothing read, the next poll tries again.
    return errcode ? 0 : n;
}

static int uart_write(void *ctx, const uint8_t *h4, size_t len) {
    bleio_adapter_obj_t *adapter = ctx;
    // Wait for CTS to go low before writing to the controller.
    uint64_t start = supervisor_ticks_ms64();
    while (adapter->cts_digitalinout != NULL &&
           common_hal_digitalio_digitalinout_get_value(adapter->cts_digitalinout)) {
        RUN_BACKGROUND_TASKS;
        if (supervisor_ticks_ms64() - start > CTS_TIMEOUT_MSECS) {
            return HCI_WRITE_TIMEOUT;
        }
    }
    int errcode = 0;
    common_hal_busio_uart_write(adapter->hci_uart, h4, len, &errcode);
    return errcode ? HCI_WRITE_ERROR : HCI_OK;
}

// RTS low: the controller may send.
static void uart_ready(void *ctx, bool ready) {
    bleio_adapter_obj_t *adapter = ctx;
    if (adapter->rts_digitalinout != NULL) {
        common_hal_digitalio_digitalinout_set_value(adapter->rts_digitalinout, !ready);
    }
}

const bleio_hci_transport_t bleio_hci_uart_transport = {
    .available = uart_available,
    .read = uart_read,
    .write = uart_write,
    .enabled = NULL,
    .ready = uart_ready,
};
