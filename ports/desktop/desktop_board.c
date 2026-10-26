// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The board is chosen at run time (--board): the board module's dictionary is mutable and points
// at the chosen table, and the pin backend behind the pins is chosen with it.

#include <stdio.h>
#include <string.h>

#include "py/objstr.h"
#include "py/qstr.h"
#include "py/runtime.h"
#include "shared-bindings/board/__init__.h"
#include "supervisor/port.h"

#include "desktop_pins.h"
#include "desktop_port.h"
#include "serial_host.h"

extern mp_map_elem_t virtual_board_table[];
extern const size_t virtual_board_table_count;
extern mp_map_elem_t ft232h_board_table[];
extern const size_t ft232h_board_table_count;
extern mp_map_elem_t mcp2221_board_table[];
extern const size_t mcp2221_board_table_count;
extern const desktop_pin_backend_t desktop_ft232h_backend;
extern const desktop_pin_backend_t desktop_mcp2221_backend;
extern mp_map_elem_t gpiochip_board_table[];
extern const size_t gpiochip_board_table_count;
extern const desktop_pin_backend_t gpiochip_backend;

typedef struct {
    const char *name;
    mp_map_elem_t *table;
    const size_t *count;
    const desktop_pin_backend_t *backend;
} desktop_board_t;

static const desktop_board_t _boards[] = {
    { "virtual", virtual_board_table, &virtual_board_table_count, &desktop_virtual_backend },
    { "ft232h", ft232h_board_table, &ft232h_board_table_count, &desktop_ft232h_backend },
    { "mcp2221", mcp2221_board_table, &mcp2221_board_table_count, &desktop_mcp2221_backend },
    { "gpiochip", gpiochip_board_table, &gpiochip_board_table_count, &gpiochip_backend },
};

// sys.platform: the host's, as CPython names it, or the real board's a board file names.
mp_obj_str_t desktop_sys_platform = {
    { &mp_type_str }, 0, sizeof(MICROPY_PY_SYS_PLATFORM) - 1, (const byte *)MICROPY_PY_SYS_PLATFORM
};

// os.uname().machine: the build's, or the real board's.
mp_obj_str_t desktop_os_machine = {
    { &mp_type_str }, 0, sizeof(MICROPY_HW_BOARD_NAME " with " MICROPY_HW_MCU_NAME) - 1,
    (const byte *)MICROPY_HW_BOARD_NAME " with " MICROPY_HW_MCU_NAME
};

static bool _set(mp_obj_str_t *str, const char *text) {
    char *copy = strdup(text);
    if (copy == NULL) {
        return false;
    }
    str->data = (const byte *)copy;
    str->len = strlen(copy);
    str->hash = qstr_compute_hash(str->data, str->len);
    return true;
}

bool desktop_set_platform(const char *name) {
    return _set(&desktop_sys_platform, name);
}

bool desktop_set_machine(const char *name) {
    return _set(&desktop_os_machine, name);
}

static const char *_board_id = "virtual";

// The banner and the web workflow name the board --board chose.
const char *port_board_id(void) {
    return _board_id;
}

bool desktop_board_select(const char *name) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(_boards); i++) {
        if (strcmp(_boards[i].name, name) == 0) {
            _board_id = _boards[i].name;
            board_module_globals.map.table = _boards[i].table;
            board_module_globals.map.used = *_boards[i].count;
            board_module_globals.map.alloc = *_boards[i].count;
            desktop_pin_backend = _boards[i].backend;
            return true;
        }
    }
    return false;
}

void desktop_board_list(void) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(_boards); i++) {
        const desktop_pin_backend_t *backend = _boards[i].backend;
        const char *state = backend->present == NULL ? "always" : backend->present() ? "connected" : "not connected";
        printf("%s\t%s\n", _boards[i].name, state);
    }
}

static mp_obj_t _board_exit(size_t n_args, const mp_obj_t *args) {
    desktop_power_off(n_args > 0 ? mp_obj_get_int(args[0]) : 0);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(desktop_board_exit_obj, 0, 1, _board_exit);

const char *desktop_uart_path(void) {
    if (desktop_options.uart_path != NULL) {
        return desktop_options.uart_path;
    }
    if (desktop_pin_backend == &gpiochip_backend) {
        return "/dev/serial0";              // a Raspberry Pi's header UART
    }
    static char path[64];
    if (desktop_pin_backend == &desktop_mcp2221_backend && serial_host_find_usb(0x04D8, 0x00DD, path, sizeof(path))) {
        return path;
    }
    return NULL;
}

const char *desktop_board_names(void) {
    static char names[64];
    if (names[0] == '\0') {
        for (size_t i = 0; i < MP_ARRAY_SIZE(_boards); i++) {
            snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s", i > 0 ? ", " : "", _boards[i].name);
        }
    }
    return names;
}
