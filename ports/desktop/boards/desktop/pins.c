// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/board/__init__.h"
#include "shared-module/displayio/__init__.h"

#include "desktop_pins.h"
#include "virtual/board_parts.h"

// The virtual board, the default. Its id is the runtime board's, not the build's (desktop).
static const MP_DEFINE_STR_OBJ(virtual_board_id_obj, "virtual");

// The buttons use the PicoPad's names, so games written for it run unchanged.
mp_map_elem_t virtual_board_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_board) },
    { MP_ROM_QSTR(MP_QSTR_board_id), MP_OBJ_FROM_PTR(&virtual_board_id_obj) },
    CIRCUITPY_BOARD_HARDWARE_KEYS

    { MP_ROM_QSTR(MP_QSTR_SW_UP), MP_OBJ_FROM_PTR(&pin_GPIO0) },
    { MP_ROM_QSTR(MP_QSTR_SW_DOWN), MP_OBJ_FROM_PTR(&pin_GPIO1) },
    { MP_ROM_QSTR(MP_QSTR_SW_LEFT), MP_OBJ_FROM_PTR(&pin_GPIO2) },
    { MP_ROM_QSTR(MP_QSTR_SW_RIGHT), MP_OBJ_FROM_PTR(&pin_GPIO3) },
    { MP_ROM_QSTR(MP_QSTR_SW_A), MP_OBJ_FROM_PTR(&pin_GPIO4) },
    { MP_ROM_QSTR(MP_QSTR_SW_B), MP_OBJ_FROM_PTR(&pin_GPIO5) },
    { MP_ROM_QSTR(MP_QSTR_SW_X), MP_OBJ_FROM_PTR(&pin_GPIO6) },
    { MP_ROM_QSTR(MP_QSTR_SW_Y), MP_OBJ_FROM_PTR(&pin_GPIO7) },
    { MP_ROM_QSTR(MP_QSTR_SW_START), MP_OBJ_FROM_PTR(&pin_GPIO8) },
    { MP_ROM_QSTR(MP_QSTR_SW_SELECT), MP_OBJ_FROM_PTR(&pin_GPIO9) },
    { MP_ROM_QSTR(MP_QSTR_SW_L), MP_OBJ_FROM_PTR(&pin_GPIO10) },
    { MP_ROM_QSTR(MP_QSTR_SW_R), MP_OBJ_FROM_PTR(&pin_GPIO11) },
    { MP_ROM_QSTR(MP_QSTR_SW_MENU), MP_OBJ_FROM_PTR(&pin_GPIO12) },

    // audioio.AudioOut(board.SPEAKER) plays through the host's sound output.
    { MP_ROM_QSTR(MP_QSTR_SPEAKER), MP_OBJ_FROM_PTR(&pin_GPIO13) },

    { MP_ROM_QSTR(MP_QSTR_TX), MP_OBJ_FROM_PTR(&pin_UART_TX) },
    { MP_ROM_QSTR(MP_QSTR_RX), MP_OBJ_FROM_PTR(&pin_UART_RX) },
    { MP_ROM_QSTR(MP_QSTR_UART), MP_OBJ_FROM_PTR(&board_uart_obj) },

    // A virtual I2C bus with the board file's devices (i2c_virtual.c); busio.I2C takes any pins.
    { MP_ROM_QSTR(MP_QSTR_SCL), MP_OBJ_FROM_PTR(&pin_BUS_SCL) },
    { MP_ROM_QSTR(MP_QSTR_SDA), MP_OBJ_FROM_PTR(&pin_BUS_SDA) },
    { MP_ROM_QSTR(MP_QSTR_I2C), MP_OBJ_FROM_PTR(&board_i2c_obj) },
    { MP_ROM_QSTR(MP_QSTR_STEMMA_I2C), MP_OBJ_FROM_PTR(&board_i2c_obj) },
    // And a virtual SPI bus with the board file's displays (spi_virtual.c).
    { MP_ROM_QSTR(MP_QSTR_SCK), MP_OBJ_FROM_PTR(&pin_BUS_SCK) },
    { MP_ROM_QSTR(MP_QSTR_MOSI), MP_OBJ_FROM_PTR(&pin_BUS_MOSI) },
    { MP_ROM_QSTR(MP_QSTR_MISO), MP_OBJ_FROM_PTR(&pin_BUS_MISO) },
    { MP_ROM_QSTR(MP_QSTR_SPI), MP_OBJ_FROM_PTR(&board_spi_obj) },

    { MP_ROM_QSTR(MP_QSTR_DISPLAY), MP_OBJ_FROM_PTR(&displays[0].framebuffer_display) },

    { MP_ROM_QSTR(MP_QSTR_exit), MP_OBJ_FROM_PTR(&desktop_board_exit_obj) },

    // Parts plugged in from Python (virtual/board_parts.c).
    { MP_ROM_QSTR(MP_QSTR_parts), MP_OBJ_FROM_PTR(&board_parts_module) },
    { MP_ROM_QSTR(MP_QSTR_attach), MP_OBJ_FROM_PTR(&board_attach_obj) },
    { MP_ROM_QSTR(MP_QSTR_detach), MP_OBJ_FROM_PTR(&board_detach_obj) },
};
const size_t virtual_board_table_count = MP_ARRAY_SIZE(virtual_board_table);

// The default; desktop_board_select() repoints it for another board.
MP_DEFINE_MUTABLE_DICT(board_module_globals, virtual_board_table);
