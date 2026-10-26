// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/board/__init__.h"
#include "shared-module/displayio/__init__.h"

#include "desktop_pins.h"

// The gpiochip board, chosen with --board gpiochip: a Raspberry Pi's header as Blinka names it, D0-D27
// being the BCM numbers (the GPIO chip's line offsets), with the I2C, SPI and UART names.
static const MP_DEFINE_STR_OBJ(gpiochip_board_id_obj, "gpiochip");

mp_map_elem_t gpiochip_board_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_board) },
    { MP_ROM_QSTR(MP_QSTR_board_id), MP_OBJ_FROM_PTR(&gpiochip_board_id_obj) },
    { MP_ROM_QSTR(MP_QSTR_D0), MP_OBJ_FROM_PTR(&pin_GPIO0) },
    { MP_ROM_QSTR(MP_QSTR_D1), MP_OBJ_FROM_PTR(&pin_GPIO1) },
    { MP_ROM_QSTR(MP_QSTR_D2), MP_OBJ_FROM_PTR(&pin_GPIO2) },
    { MP_ROM_QSTR(MP_QSTR_D3), MP_OBJ_FROM_PTR(&pin_GPIO3) },
    { MP_ROM_QSTR(MP_QSTR_D4), MP_OBJ_FROM_PTR(&pin_GPIO4) },
    { MP_ROM_QSTR(MP_QSTR_D5), MP_OBJ_FROM_PTR(&pin_GPIO5) },
    { MP_ROM_QSTR(MP_QSTR_D6), MP_OBJ_FROM_PTR(&pin_GPIO6) },
    { MP_ROM_QSTR(MP_QSTR_D7), MP_OBJ_FROM_PTR(&pin_GPIO7) },
    { MP_ROM_QSTR(MP_QSTR_D8), MP_OBJ_FROM_PTR(&pin_GPIO8) },
    { MP_ROM_QSTR(MP_QSTR_D9), MP_OBJ_FROM_PTR(&pin_GPIO9) },
    { MP_ROM_QSTR(MP_QSTR_D10), MP_OBJ_FROM_PTR(&pin_GPIO10) },
    { MP_ROM_QSTR(MP_QSTR_D11), MP_OBJ_FROM_PTR(&pin_GPIO11) },
    { MP_ROM_QSTR(MP_QSTR_D12), MP_OBJ_FROM_PTR(&pin_GPIO12) },
    { MP_ROM_QSTR(MP_QSTR_D13), MP_OBJ_FROM_PTR(&pin_GPIO13) },
    { MP_ROM_QSTR(MP_QSTR_D14), MP_OBJ_FROM_PTR(&pin_GPIO14) },
    { MP_ROM_QSTR(MP_QSTR_D15), MP_OBJ_FROM_PTR(&pin_GPIO15) },
    { MP_ROM_QSTR(MP_QSTR_D16), MP_OBJ_FROM_PTR(&pin_GPIO16) },
    { MP_ROM_QSTR(MP_QSTR_D17), MP_OBJ_FROM_PTR(&pin_GPIO17) },
    { MP_ROM_QSTR(MP_QSTR_D18), MP_OBJ_FROM_PTR(&pin_GPIO18) },
    { MP_ROM_QSTR(MP_QSTR_D19), MP_OBJ_FROM_PTR(&pin_GPIO19) },
    { MP_ROM_QSTR(MP_QSTR_D20), MP_OBJ_FROM_PTR(&pin_GPIO20) },
    { MP_ROM_QSTR(MP_QSTR_D21), MP_OBJ_FROM_PTR(&pin_GPIO21) },
    { MP_ROM_QSTR(MP_QSTR_D22), MP_OBJ_FROM_PTR(&pin_GPIO22) },
    { MP_ROM_QSTR(MP_QSTR_D23), MP_OBJ_FROM_PTR(&pin_GPIO23) },
    { MP_ROM_QSTR(MP_QSTR_D24), MP_OBJ_FROM_PTR(&pin_GPIO24) },
    { MP_ROM_QSTR(MP_QSTR_D25), MP_OBJ_FROM_PTR(&pin_GPIO25) },
    { MP_ROM_QSTR(MP_QSTR_D26), MP_OBJ_FROM_PTR(&pin_GPIO26) },
    { MP_ROM_QSTR(MP_QSTR_D27), MP_OBJ_FROM_PTR(&pin_GPIO27) },

    { MP_ROM_QSTR(MP_QSTR_SDA), MP_OBJ_FROM_PTR(&pin_GPIO2) },
    { MP_ROM_QSTR(MP_QSTR_SCL), MP_OBJ_FROM_PTR(&pin_GPIO3) },
    { MP_ROM_QSTR(MP_QSTR_CE1), MP_OBJ_FROM_PTR(&pin_GPIO7) },
    { MP_ROM_QSTR(MP_QSTR_CE0), MP_OBJ_FROM_PTR(&pin_GPIO8) },
    { MP_ROM_QSTR(MP_QSTR_MISO), MP_OBJ_FROM_PTR(&pin_GPIO9) },
    { MP_ROM_QSTR(MP_QSTR_MOSI), MP_OBJ_FROM_PTR(&pin_GPIO10) },
    { MP_ROM_QSTR(MP_QSTR_SCK), MP_OBJ_FROM_PTR(&pin_GPIO11) },
    { MP_ROM_QSTR(MP_QSTR_SCLK), MP_OBJ_FROM_PTR(&pin_GPIO11) },

    { MP_ROM_QSTR(MP_QSTR_TX), MP_OBJ_FROM_PTR(&pin_UART_TX) },
    { MP_ROM_QSTR(MP_QSTR_RX), MP_OBJ_FROM_PTR(&pin_UART_RX) },
    { MP_ROM_QSTR(MP_QSTR_UART), MP_OBJ_FROM_PTR(&board_uart_obj) },

    { MP_ROM_QSTR(MP_QSTR_DISPLAY), MP_OBJ_FROM_PTR(&displays[0].framebuffer_display) },

    { MP_ROM_QSTR(MP_QSTR_exit), MP_OBJ_FROM_PTR(&desktop_board_exit_obj) },

    { MP_ROM_QSTR(MP_QSTR_I2C), MP_OBJ_FROM_PTR(&board_i2c_obj) },
    { MP_ROM_QSTR(MP_QSTR_SPI), MP_OBJ_FROM_PTR(&board_spi_obj) },
};
const size_t gpiochip_board_table_count = MP_ARRAY_SIZE(gpiochip_board_table);
