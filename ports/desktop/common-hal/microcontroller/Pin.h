// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// A pin is a number; what is behind it is the pin backend (desktop_pins.h): the virtual pins the
// console's buttons drive, or a transport to real GPIO.
typedef struct {
    mp_obj_base_t base;
    uint8_t number;
} mcu_pin_obj_t;

// GPIO numbers 0-27: an FT232H uses 0-15, a Raspberry Pi's header 0-27 (BCM numbering).
#define DESKTOP_PIN_COUNT 28

extern const mcu_pin_obj_t pin_GPIO0;
extern const mcu_pin_obj_t pin_GPIO1;
extern const mcu_pin_obj_t pin_GPIO2;
extern const mcu_pin_obj_t pin_GPIO3;
extern const mcu_pin_obj_t pin_GPIO4;
extern const mcu_pin_obj_t pin_GPIO5;
extern const mcu_pin_obj_t pin_GPIO6;
extern const mcu_pin_obj_t pin_GPIO7;
extern const mcu_pin_obj_t pin_GPIO8;
extern const mcu_pin_obj_t pin_GPIO9;
extern const mcu_pin_obj_t pin_GPIO10;
extern const mcu_pin_obj_t pin_GPIO11;
extern const mcu_pin_obj_t pin_GPIO12;
extern const mcu_pin_obj_t pin_GPIO13;
extern const mcu_pin_obj_t pin_GPIO14;
extern const mcu_pin_obj_t pin_GPIO15;
extern const mcu_pin_obj_t pin_GPIO16;
extern const mcu_pin_obj_t pin_GPIO17;
extern const mcu_pin_obj_t pin_GPIO18;
extern const mcu_pin_obj_t pin_GPIO19;
extern const mcu_pin_obj_t pin_GPIO20;
extern const mcu_pin_obj_t pin_GPIO21;
extern const mcu_pin_obj_t pin_GPIO22;
extern const mcu_pin_obj_t pin_GPIO23;
extern const mcu_pin_obj_t pin_GPIO24;
extern const mcu_pin_obj_t pin_GPIO25;
extern const mcu_pin_obj_t pin_GPIO26;
extern const mcu_pin_obj_t pin_GPIO27;

// A host serial port's lines (busio.UART): numbered past the GPIO, so nothing configures them.
#define DESKTOP_PIN_UART_TX 30
#define DESKTOP_PIN_UART_RX 31
extern const mcu_pin_obj_t pin_UART_TX;
extern const mcu_pin_obj_t pin_UART_RX;

// What board.I2C() and board.SPI() pass: busio takes them as the chosen board's bus pins.
#define DESKTOP_PIN_BUS_SCL 32
#define DESKTOP_PIN_BUS_SDA 33
#define DESKTOP_PIN_BUS_SCK 34
#define DESKTOP_PIN_BUS_MOSI 35
#define DESKTOP_PIN_BUS_MISO 36
extern const mcu_pin_obj_t pin_BUS_SCL;
extern const mcu_pin_obj_t pin_BUS_SDA;
extern const mcu_pin_obj_t pin_BUS_SCK;
extern const mcu_pin_obj_t pin_BUS_MOSI;
extern const mcu_pin_obj_t pin_BUS_MISO;

// The virtual board's virtual pins: GPIO 0-27, and the bus and serial pins above (30-36), which
// a program may also use as plain pins (DigitalInOut(board.TX)), as on a microcontroller.
#define DESKTOP_VIRTUAL_PIN_COUNT (DESKTOP_PIN_BUS_MISO + 1)

// A pin digitalio, touchio and the like take: a GPIO, or on the virtual board a bus pin too.
bool desktop_pin_is_gpio(uint8_t number);

void reset_all_pins(void);
void common_hal_reset_pin(const mcu_pin_obj_t *pin);
uint8_t common_hal_mcu_pin_number(const mcu_pin_obj_t *pin);
void common_hal_mcu_pin_claim(const mcu_pin_obj_t *pin);
