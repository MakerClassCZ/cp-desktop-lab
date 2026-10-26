// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/board/__init__.h"
#include "shared-module/displayio/__init__.h"

#include "desktop_pins.h"

// The FT232H board, chosen with --board ft232h: pins 0-7 are ADBUS0-7 (D0-D7), 8-15 ACBUS0-7
// (C0-C7). The names follow Blinka's FT232H so examples written for it read the same.
static const MP_DEFINE_STR_OBJ(ft232h_board_id_obj, "ft232h");

mp_map_elem_t ft232h_board_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_board) },
    { MP_ROM_QSTR(MP_QSTR_board_id), MP_OBJ_FROM_PTR(&ft232h_board_id_obj) },

    { MP_ROM_QSTR(MP_QSTR_D0), MP_OBJ_FROM_PTR(&pin_GPIO0) },
    { MP_ROM_QSTR(MP_QSTR_D1), MP_OBJ_FROM_PTR(&pin_GPIO1) },
    { MP_ROM_QSTR(MP_QSTR_D2), MP_OBJ_FROM_PTR(&pin_GPIO2) },
    { MP_ROM_QSTR(MP_QSTR_D3), MP_OBJ_FROM_PTR(&pin_GPIO3) },
    { MP_ROM_QSTR(MP_QSTR_D4), MP_OBJ_FROM_PTR(&pin_GPIO4) },
    { MP_ROM_QSTR(MP_QSTR_D5), MP_OBJ_FROM_PTR(&pin_GPIO5) },
    { MP_ROM_QSTR(MP_QSTR_D6), MP_OBJ_FROM_PTR(&pin_GPIO6) },
    { MP_ROM_QSTR(MP_QSTR_D7), MP_OBJ_FROM_PTR(&pin_GPIO7) },
    { MP_ROM_QSTR(MP_QSTR_C0), MP_OBJ_FROM_PTR(&pin_GPIO8) },
    { MP_ROM_QSTR(MP_QSTR_C1), MP_OBJ_FROM_PTR(&pin_GPIO9) },
    { MP_ROM_QSTR(MP_QSTR_C2), MP_OBJ_FROM_PTR(&pin_GPIO10) },
    { MP_ROM_QSTR(MP_QSTR_C3), MP_OBJ_FROM_PTR(&pin_GPIO11) },
    { MP_ROM_QSTR(MP_QSTR_C4), MP_OBJ_FROM_PTR(&pin_GPIO12) },
    { MP_ROM_QSTR(MP_QSTR_C5), MP_OBJ_FROM_PTR(&pin_GPIO13) },
    { MP_ROM_QSTR(MP_QSTR_C6), MP_OBJ_FROM_PTR(&pin_GPIO14) },
    { MP_ROM_QSTR(MP_QSTR_C7), MP_OBJ_FROM_PTR(&pin_GPIO15) },

    { MP_ROM_QSTR(MP_QSTR_SCK), MP_OBJ_FROM_PTR(&pin_GPIO0) },
    { MP_ROM_QSTR(MP_QSTR_SCL), MP_OBJ_FROM_PTR(&pin_GPIO0) },
    { MP_ROM_QSTR(MP_QSTR_MOSI), MP_OBJ_FROM_PTR(&pin_GPIO1) },
    { MP_ROM_QSTR(MP_QSTR_SDA), MP_OBJ_FROM_PTR(&pin_GPIO1) },
    { MP_ROM_QSTR(MP_QSTR_MISO), MP_OBJ_FROM_PTR(&pin_GPIO2) },

    { MP_ROM_QSTR(MP_QSTR_TX), MP_OBJ_FROM_PTR(&pin_UART_TX) },
    { MP_ROM_QSTR(MP_QSTR_RX), MP_OBJ_FROM_PTR(&pin_UART_RX) },
    { MP_ROM_QSTR(MP_QSTR_UART), MP_OBJ_FROM_PTR(&board_uart_obj) },

    { MP_ROM_QSTR(MP_QSTR_DISPLAY), MP_OBJ_FROM_PTR(&displays[0].framebuffer_display) },

    { MP_ROM_QSTR(MP_QSTR_exit), MP_OBJ_FROM_PTR(&desktop_board_exit_obj) },

    { MP_ROM_QSTR(MP_QSTR_SPI), MP_OBJ_FROM_PTR(&board_spi_obj) },
    { MP_ROM_QSTR(MP_QSTR_I2C), MP_OBJ_FROM_PTR(&board_i2c_obj) },
};
const size_t ft232h_board_table_count = MP_ARRAY_SIZE(ft232h_board_table);
