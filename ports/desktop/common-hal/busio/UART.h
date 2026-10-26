// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "common-hal/microcontroller/Pin.h"

#include "serial_host.h"

// busio.UART on a serial port of the host (--uart, or the MCP2221's own): board.TX and board.RX.
typedef struct {
    mp_obj_base_t base;
    serial_host_t port;
    const mcu_pin_obj_t *tx;
    const mcu_pin_obj_t *rx;
    uint32_t baudrate;
    uint32_t timeout_ms;
    uint8_t bits;
    uint8_t stop;
    char parity;                // 'N', 'E' or 'O'
    bool open;
    bool virtual_port;          // the virtual board with no --uart: nothing on the other end
} busio_uart_obj_t;
