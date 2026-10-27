// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "shared-bindings/digitalio/DigitalInOut.h"

// A bus a backend offers: the pins are checked and claimed by common-hal/busio before these run.
#define DESKTOP_BUS_ANY_PIN 0xff          // a virtual bus: on any pins
typedef struct {
    uint8_t scl, sda;                   // the pin numbers the bus is on, or DESKTOP_BUS_ANY_PIN
    bool (*construct)(uint32_t frequency, uint32_t *actual_hz);
    void (*deinit)(void);
    int (*write)(uint16_t address, const uint8_t *data, size_t len, bool stop);   // 0 or -MP_E*
    int (*read)(uint16_t address, uint8_t *data, size_t len);                     // after write(stop=false): a repeated start
} desktop_i2c_ops_t;

typedef enum {
    DESKTOP_SPI_OK,
    DESKTOP_SPI_BAD_BITS,                 // busio returns False, as the API asks
    DESKTOP_SPI_BAD_MODE,                 // the clock phase or polarity: a ValueError
    DESKTOP_SPI_FAILED,                   // the device refused the settings: an OSError
} desktop_spi_result_t;

typedef struct {
    uint8_t sck, mosi, miso;            // the pin numbers the bus is on, or DESKTOP_BUS_ANY_PIN
    bool (*construct)(bool has_mosi, bool has_miso);    // false = the device is lost
    void (*deinit)(void);
    desktop_spi_result_t (*configure)(uint32_t hz, uint8_t polarity, uint8_t phase, uint8_t bits, uint32_t *actual_hz);
    // len bytes: out (or fill when NULL) goes out, in (when not NULL) comes in. false = failed.
    bool (*transfer)(const uint8_t *out, uint8_t fill, uint8_t *in, size_t len);
} desktop_spi_ops_t;

// What is behind the pins: the virtual pins the console's buttons drive, or a transport to real
// GPIO. No entry raises: reset runs outside any exception handler.
typedef struct {
    const char *name;
    bool (*present)(void);              // the adapter is plugged in (not opened); NULL = always there
    bool (*open)(void);                 // at start-up; false = cannot start, the cause is printed
    void (*close)(void);
    void (*reset)(uint32_t keep_mask);  // every pin outside keep_mask becomes an input
    digitalinout_result_t (*configure)(uint8_t pin, bool output, bool open_drain, digitalio_pull_t pull);
    bool (*write)(uint8_t pin, bool value);    // false = device lost
    bool (*read)(uint8_t pin, bool *level);    // false = device lost
    bool (*lost)(void);                        // NULL = never
    bool (*pin_ok)(uint8_t pin);               // digitalio may take this pin; NULL = every pin
    // A pin given up (a deinit): back to how the board had it, without taking it over when it was
    // never ours. NULL = configure it as an input.
    void (*release)(uint8_t pin);
    const desktop_i2c_ops_t *i2c;                // NULL = no I2C on this backend
    const desktop_spi_ops_t *spi;                // NULL = no SPI on this backend
    bool accepts_drive;                        // desktop_pin_drive() (the console's buttons) applies
    // NeoPixel (WS2812) data on a pin, as neopixel_write gives it: NULL where none goes out
    // (the panel shows them); false with *why set when this pin cannot carry it.
    bool (*neopixel)(uint8_t pin, const uint8_t *pixels, size_t len, const char **why);
} desktop_pin_backend_t;

extern const desktop_pin_backend_t *desktop_pin_backend;
// Why a bus's construct() failed, for busio's exception; empty when it did not say.
extern char desktop_bus_error[96];
// A bus that would not construct: OSError with the backend's reason, or EIO.
NORETURN void desktop_raise_bus_error(void);
extern const desktop_pin_backend_t desktop_virtual_backend;

bool desktop_pin_backend_lost(void);
// OSError("FT232H lost; restart the program") when it is.
void desktop_pin_backend_raise_if_lost(void);

// The pin object of a GPIO number, and a bus pin the way busio takes it: board.I2C() and
// board.SPI() pass placeholders (pin_BUS_SCL ...) that stand for the chosen board's bus pins.
const mcu_pin_obj_t *desktop_pin_obj(uint8_t number);
const mcu_pin_obj_t *desktop_bus_pin(const mcu_pin_obj_t *pin);

// board.exit(status=0): the way out of code or the REPL on a host, on every board (desktop_board.c).
extern const mp_obj_fun_builtin_var_t desktop_board_exit_obj;

// A host button sets the level a virtual pin reads.
void desktop_pin_drive(uint8_t number, bool level);
// And lets it go: the pin reads its pull again.
void desktop_pin_release(uint8_t number);
// A virtual pin's direction and level, for the board panel; false off the virtual board.
bool desktop_virtual_pin(uint8_t number, bool *output, bool *level);
