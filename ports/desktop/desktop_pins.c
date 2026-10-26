// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "desktop_pins.h"

#include <string.h>

#include "py/runtime.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "i2c_virtual.h"
#include "spi_virtual.h"
#include "desktop_port.h"
#include "parts.h"

#define PIN(n) const mcu_pin_obj_t pin_GPIO##n = { .base = { .type = &mcu_pin_type }, .number = n }
PIN(0);
PIN(1);
PIN(2);
PIN(3);
PIN(4);
PIN(5);
PIN(6);
PIN(7);
PIN(8);
PIN(9);
PIN(10);
PIN(11);
PIN(12);
PIN(13);
PIN(14);
PIN(15);
PIN(16);
PIN(17);
PIN(18);
PIN(19);
PIN(20);
PIN(21);
PIN(22);
PIN(23);
PIN(24);
PIN(25);
PIN(26);
PIN(27);
const mcu_pin_obj_t pin_UART_TX = { .base = { .type = &mcu_pin_type }, .number = DESKTOP_PIN_UART_TX };
const mcu_pin_obj_t pin_UART_RX = { .base = { .type = &mcu_pin_type }, .number = DESKTOP_PIN_UART_RX };
#define BUS_PIN(name) const mcu_pin_obj_t pin_BUS_##name = { .base = { .type = &mcu_pin_type }, .number = DESKTOP_PIN_BUS_##name }
BUS_PIN(SCL);
BUS_PIN(SDA);
BUS_PIN(SCK);
BUS_PIN(MOSI);
BUS_PIN(MISO);

static const mcu_pin_obj_t *const _gpio[DESKTOP_PIN_COUNT] = {
    &pin_GPIO0, &pin_GPIO1, &pin_GPIO2, &pin_GPIO3, &pin_GPIO4, &pin_GPIO5, &pin_GPIO6, &pin_GPIO7,
    &pin_GPIO8, &pin_GPIO9, &pin_GPIO10, &pin_GPIO11, &pin_GPIO12, &pin_GPIO13, &pin_GPIO14, &pin_GPIO15,
    &pin_GPIO16, &pin_GPIO17, &pin_GPIO18, &pin_GPIO19, &pin_GPIO20, &pin_GPIO21, &pin_GPIO22, &pin_GPIO23,
    &pin_GPIO24, &pin_GPIO25, &pin_GPIO26, &pin_GPIO27,
};

static const mcu_pin_obj_t *const _virtual_only[] = {
    &pin_UART_TX, &pin_UART_RX, &pin_BUS_SCL, &pin_BUS_SDA, &pin_BUS_SCK, &pin_BUS_MOSI, &pin_BUS_MISO,
};

const mcu_pin_obj_t *desktop_pin_obj(uint8_t number) {
    if (number < DESKTOP_PIN_COUNT) {
        return _gpio[number];
    }
    return desktop_pin_is_gpio(number) ? _virtual_only[number - DESKTOP_PIN_UART_TX] : NULL;
}

const mcu_pin_obj_t *desktop_bus_pin(const mcu_pin_obj_t *pin) {
    const desktop_i2c_ops_t *i2c = desktop_pin_backend->i2c;
    const desktop_spi_ops_t *spi = desktop_pin_backend->spi;
    switch (pin != NULL ? pin->number : 0) {
        case DESKTOP_PIN_BUS_SCL:
            return i2c != NULL && i2c->scl != DESKTOP_BUS_ANY_PIN ? _gpio[i2c->scl] : pin;
        case DESKTOP_PIN_BUS_SDA:
            return i2c != NULL && i2c->sda != DESKTOP_BUS_ANY_PIN ? _gpio[i2c->sda] : pin;
        case DESKTOP_PIN_BUS_SCK:
            return spi != NULL && spi->sck != DESKTOP_BUS_ANY_PIN ? _gpio[spi->sck] : pin;
        case DESKTOP_PIN_BUS_MOSI:
            return spi != NULL && spi->mosi != DESKTOP_BUS_ANY_PIN ? _gpio[spi->mosi] : pin;
        case DESKTOP_PIN_BUS_MISO:
            return spi != NULL && spi->miso != DESKTOP_BUS_ANY_PIN ? _gpio[spi->miso] : pin;
        default:
            return pin;
    }
}

// The virtual pins: a level kept in memory, driven by the console's buttons.
typedef struct {
    bool output;
    bool out_value;
    uint8_t pull;           // digitalio_pull_t
    bool driven;            // something outside drives the pin (a button)
    bool driven_level;
} virtual_pin_t;

static virtual_pin_t _pins[DESKTOP_VIRTUAL_PIN_COUNT];

static bool _virtual_open(void) {
    return true;
}

static void _virtual_close(void) {
}

static void _virtual_reset(uint32_t keep_mask) {
    for (uint8_t i = 0; i < DESKTOP_VIRTUAL_PIN_COUNT; i++) {
        if (i >= 32 || !(keep_mask & (1u << i))) {
            _pins[i].output = false;
            _pins[i].out_value = false;
            _pins[i].pull = PULL_NONE;
            parts_pin_changed(i);
        }
    }
}

static digitalinout_result_t _virtual_configure(uint8_t pin, bool output, bool open_drain, digitalio_pull_t pull) {
    (void)open_drain;
    _pins[pin].output = output;
    _pins[pin].pull = pull;
    parts_pin_changed(pin);
    return DIGITALINOUT_OK;
}

static bool _virtual_write(uint8_t pin, bool value) {
    _pins[pin].out_value = value;
    parts_pin_changed(pin);
    return true;
}

static bool _virtual_read(uint8_t pin, bool *level) {
    virtual_pin_t *p = &_pins[pin];
    if (!p->output && parts_pin_level(pin, level)) {
        return true;
    }
    if (p->output) {
        *level = p->out_value;
    } else if (p->driven) {
        *level = p->driven_level;
    } else {
        *level = p->pull == PULL_UP;
    }
    return true;
}

const desktop_pin_backend_t desktop_virtual_backend = {
    .name = "virtual",
    .present = NULL,
    .open = _virtual_open,
    .close = _virtual_close,
    .reset = _virtual_reset,
    .configure = _virtual_configure,
    .write = _virtual_write,
    .read = _virtual_read,
    .lost = NULL,
    .i2c = &i2c_virtual_ops,
    .spi = &spi_virtual_ops,
    .accepts_drive = true,
};

const desktop_pin_backend_t *desktop_pin_backend = &desktop_virtual_backend;
char desktop_bus_error[96];

void desktop_raise_bus_error(void) {
    char why[sizeof(desktop_bus_error)];
    memcpy(why, desktop_bus_error, sizeof(why));
    desktop_bus_error[0] = '\0';
    if (why[0] != '\0') {
        mp_raise_OSError_msg_varg(MP_ERROR_TEXT("%s"), why);
    }
    mp_raise_OSError(MP_EIO);
}

bool desktop_pin_backend_lost(void) {
    return desktop_pin_backend->lost != NULL && desktop_pin_backend->lost();
}

void desktop_pin_backend_raise_if_lost(void) {
    if (desktop_pin_backend_lost()) {
        mp_raise_OSError_msg_varg(MP_ERROR_TEXT("%s lost; restart the program"), desktop_pin_backend->name);
    }
}

void desktop_pin_drive(uint8_t number, bool level) {
    if (desktop_pin_backend->accepts_drive && number < DESKTOP_VIRTUAL_PIN_COUNT) {
        _pins[number].driven = true;
        _pins[number].driven_level = level;
    }
}

bool desktop_pin_is_gpio(uint8_t number) {
    return number < DESKTOP_PIN_COUNT
           || (desktop_pin_backend == &desktop_virtual_backend && number >= DESKTOP_PIN_UART_TX && number < DESKTOP_VIRTUAL_PIN_COUNT);
}

bool desktop_virtual_pin(uint8_t number, bool *output, bool *level) {
    if (desktop_pin_backend != &desktop_virtual_backend || !desktop_pin_is_gpio(number)) {
        return false;
    }
    *output = _pins[number].output;
    return _virtual_read(number, level);
}

void desktop_button_set(int button, bool pressed) {
    desktop_pin_drive(button, !pressed);
}

bool desktop_button_held(int button) {
    return button >= 0 && button < DESKTOP_BUTTON_COUNT && _pins[button].driven && !_pins[button].driven_level;
}

void desktop_buttons_release_all(void) {
    for (int i = 0; i < DESKTOP_BUTTON_COUNT; i++) {
        desktop_button_set(i, false);
    }
}
