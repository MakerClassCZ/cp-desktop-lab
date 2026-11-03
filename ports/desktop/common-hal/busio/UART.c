// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// busio.UART on a serial port of the host: --uart PORT on any board, else the MCP2221's own
// serial port on --board mcp2221. board.TX and board.RX stand for the port's two lines; on the
// virtual board any two free pins do, and with no --uart the port is virtual: what the program
// writes goes to the bus trace, and nothing ever arrives.

#include "py/mpconfig.h"

#if CIRCUITPY_BUSIO_UART

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "shared/runtime/interrupt_char.h"
#include "shared-bindings/busio/UART.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "bus_trace.h"
#include "desktop_pins.h"
#include "desktop_port.h"
#include "serial_host.h"

#define SLICE_MS 20             // a long read waits in slices, so Ctrl-C and background tasks run

void common_hal_busio_uart_construct(busio_uart_obj_t *self,
    const mcu_pin_obj_t *tx, const mcu_pin_obj_t *rx,
    const mcu_pin_obj_t *rts, const mcu_pin_obj_t *cts,
    const mcu_pin_obj_t *rs485_dir, bool rs485_invert,
    uint32_t baudrate, uint8_t bits, busio_uart_parity_t parity, uint8_t stop,
    mp_float_t timeout, uint16_t receiver_buffer_size, byte *receiver_buffer,
    bool sigint_enabled) {
    (void)rs485_invert;
    (void)receiver_buffer_size;
    (void)receiver_buffer;
    (void)sigint_enabled;
    if (rts != NULL || cts != NULL || rs485_dir != NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("no flow control or RS485 on a host serial port"));
    }
    bool virtual_board = desktop_pin_backend == &desktop_virtual_backend;
    if (tx == &pin_UART_RX || rx == &pin_UART_TX
        || (!virtual_board && ((tx != NULL && tx != &pin_UART_TX) || (rx != NULL && rx != &pin_UART_RX)))) {
        raise_ValueError_invalid_pins();
    }
    if (tx != NULL) {
        assert_pin_free(tx);
    }
    if (rx != NULL) {
        assert_pin_free(rx);
    }
    if (bits > 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("bits must be 5 to 8"));
    }
    const char *path = desktop_uart_path();
    if (path == NULL && !virtual_board) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("busio.UART needs --uart PORT"));
    }
    self->virtual_port = path == NULL;
    char err[256];
    if (!self->virtual_port && !serial_host_open(&self->port, path, err, sizeof(err))) {
        mp_raise_OSError_msg_varg(MP_ERROR_TEXT("%s"), err);
    }
    char p = parity == BUSIO_UART_PARITY_EVEN ? 'E' : parity == BUSIO_UART_PARITY_ODD ? 'O' : 'N';
    if (!self->virtual_port && !serial_host_configure(&self->port, baudrate, bits, p, stop)) {
        serial_host_close(&self->port);
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%s cannot run at %d baud"), path, (int)baudrate);
    }
    self->tx = tx;
    self->rx = rx;
    self->baudrate = baudrate;
    self->bits = bits;
    self->parity = p;
    self->stop = stop;
    self->timeout_ms = (uint32_t)(timeout * 1000);
    self->open = true;
    if (tx != NULL) {
        common_hal_mcu_pin_claim(tx);
    }
    if (rx != NULL) {
        common_hal_mcu_pin_claim(rx);
    }
}

bool common_hal_busio_uart_deinited(busio_uart_obj_t *self) {
    return !self->open;
}

void common_hal_busio_uart_deinit(busio_uart_obj_t *self) {
    if (!self->open) {
        return;
    }
    if (!self->virtual_port) {
        serial_host_close(&self->port);
    }
    common_hal_reset_pin(self->tx);
    common_hal_reset_pin(self->rx);
    self->tx = NULL;
    self->rx = NULL;
    self->open = false;
}

#if CIRCUITPY_BULK_RESET
void common_hal_busio_uart_never_reset(busio_uart_obj_t *self) {
    if (self->tx != NULL) {
        common_hal_never_reset_pin(self->tx);
    }
    if (self->rx != NULL) {
        common_hal_never_reset_pin(self->rx);
    }
}
#endif

// Up to len bytes: until they are all there, or timeout passes with no new byte.
size_t common_hal_busio_uart_read(busio_uart_obj_t *self, uint8_t *data, size_t len, int *errcode) {
    if (self->rx == NULL) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("No %q pin"), MP_QSTR_rx);
    }
    if (len == 0) {
        return 0;
    }
    size_t got = 0;
    uint64_t last = mp_hal_ticks_ms();
    for (;;) {
        uint64_t waited = mp_hal_ticks_ms() - last;
        int slice = waited >= self->timeout_ms ? 0 : (int)(self->timeout_ms - waited);
        if (slice > SLICE_MS) {
            slice = SLICE_MS;
        }
        int n = 0;
        if (self->virtual_port) {
            mp_hal_delay_ms(slice);
        } else {
            n = serial_host_read(&self->port, data + got, len - got, slice);
        }
        if (n < 0) {
            *errcode = MP_EIO;
            return MP_STREAM_ERROR;
        }
        if (n > 0) {
            got += n;
            last = mp_hal_ticks_ms();
        }
        if (got == len || (n == 0 && mp_hal_ticks_ms() - last >= self->timeout_ms)) {
            break;
        }
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted()) {
            break;
        }
    }
    bus_trace_uart(false, data, got);
    if (got == 0) {
        *errcode = MP_EAGAIN;
        return MP_STREAM_ERROR;
    }
    return got;
}

size_t common_hal_busio_uart_write(busio_uart_obj_t *self, const uint8_t *data, size_t len, int *errcode) {
    if (self->tx == NULL) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("No %q pin"), MP_QSTR_tx);
    }
    // Long enough for the bytes at the baud rate, and a second more.
    int timeout = 1000 + (int)((uint64_t)len * 10 * 1000 / (self->baudrate != 0 ? self->baudrate : 9600));
    int n = (int)len;
    if (self->virtual_port) {
        // As long as the bytes take at the baud rate.
        mp_hal_delay_ms((uint32_t)((uint64_t)len * 10 * 1000 / (self->baudrate != 0 ? self->baudrate : 9600)));
    } else {
        n = serial_host_write(&self->port, data, len, timeout);
    }
    if (n < 0) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    bus_trace_uart(true, data, n);
    return n;
}

uint32_t common_hal_busio_uart_get_baudrate(busio_uart_obj_t *self) {
    return self->baudrate;
}

void common_hal_busio_uart_set_baudrate(busio_uart_obj_t *self, uint32_t baudrate) {
    if (!self->virtual_port && !serial_host_configure(&self->port, baudrate, self->bits, self->parity, self->stop)) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("cannot run at %d baud"), (int)baudrate);
    }
    self->baudrate = baudrate;
}

mp_float_t common_hal_busio_uart_get_timeout(busio_uart_obj_t *self) {
    return (mp_float_t)self->timeout_ms / 1000;
}

void common_hal_busio_uart_set_timeout(busio_uart_obj_t *self, mp_float_t timeout) {
    self->timeout_ms = (uint32_t)(timeout * 1000);
}

uint32_t common_hal_busio_uart_rx_characters_available(busio_uart_obj_t *self) {
    int n = self->virtual_port ? 0 : serial_host_available(&self->port);
    return n > 0 ? (uint32_t)n : 0;
}

void common_hal_busio_uart_clear_rx_buffer(busio_uart_obj_t *self) {
    if (!self->virtual_port) {
        serial_host_flush_input(&self->port);
    }
}

bool common_hal_busio_uart_ready_to_tx(busio_uart_obj_t *self) {
    return self->open;
}

#endif
