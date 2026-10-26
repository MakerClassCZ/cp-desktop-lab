// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/board/__init__.h"
#include "shared-module/displayio/__init__.h"

#include "desktop_pins.h"

// The MCP2221A board, chosen with --board mcp2221: SCL and SDA are the chip's I2C pins (0 and 1,
// the same numbers as on the FT232H board, so board.I2C() serves both), G0-G3 its GPIO (2-5).
static const MP_DEFINE_STR_OBJ(mcp2221_board_id_obj, "mcp2221");

mp_map_elem_t mcp2221_board_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_board) },
    { MP_ROM_QSTR(MP_QSTR_board_id), MP_OBJ_FROM_PTR(&mcp2221_board_id_obj) },

    { MP_ROM_QSTR(MP_QSTR_SCL), MP_OBJ_FROM_PTR(&pin_GPIO0) },
    { MP_ROM_QSTR(MP_QSTR_SDA), MP_OBJ_FROM_PTR(&pin_GPIO1) },
    { MP_ROM_QSTR(MP_QSTR_G0), MP_OBJ_FROM_PTR(&pin_GPIO2) },
    { MP_ROM_QSTR(MP_QSTR_G1), MP_OBJ_FROM_PTR(&pin_GPIO3) },
    { MP_ROM_QSTR(MP_QSTR_G2), MP_OBJ_FROM_PTR(&pin_GPIO4) },
    { MP_ROM_QSTR(MP_QSTR_G3), MP_OBJ_FROM_PTR(&pin_GPIO5) },

    { MP_ROM_QSTR(MP_QSTR_TX), MP_OBJ_FROM_PTR(&pin_UART_TX) },
    { MP_ROM_QSTR(MP_QSTR_RX), MP_OBJ_FROM_PTR(&pin_UART_RX) },
    { MP_ROM_QSTR(MP_QSTR_UART), MP_OBJ_FROM_PTR(&board_uart_obj) },

    { MP_ROM_QSTR(MP_QSTR_DISPLAY), MP_OBJ_FROM_PTR(&displays[0].framebuffer_display) },

    { MP_ROM_QSTR(MP_QSTR_exit), MP_OBJ_FROM_PTR(&desktop_board_exit_obj) },

    { MP_ROM_QSTR(MP_QSTR_I2C), MP_OBJ_FROM_PTR(&board_i2c_obj) },
};
const size_t mcp2221_board_table_count = MP_ARRAY_SIZE(mcp2221_board_table);
