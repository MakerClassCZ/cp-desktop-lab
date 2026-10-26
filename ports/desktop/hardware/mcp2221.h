// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "desktop_pins.h"

// An MCP2221A over HID: four GPIO (pins 2-5 = GP0-GP3), the I2C engine (pins 0-1 = SCL, SDA),
// three ADC channels on GP1-GP3 and one DAC on GP2 or GP3.

#define MCP2221_PIN_SCL 0
#define MCP2221_PIN_SDA 1
#define MCP2221_PIN_GP0 2
#define MCP2221_PIN_COUNT 6

// Pin designations, as the chip's SRAM settings number them.
#define MCP2221_GPIO 0
#define MCP2221_ADC 2
#define MCP2221_DAC 3

extern const desktop_pin_backend_t desktop_mcp2221_backend;

bool mcp2221_lost(void);

// A GP's function (GPIO, ADC, DAC); GPIO keeps the pin's direction and level.
bool mcp2221_set_designation(uint8_t gp, uint8_t designation);
bool mcp2221_adc_read(uint8_t channel, uint16_t *value);      // channel 1-3 (GP1-GP3), 10 bits
bool mcp2221_dac_set(uint8_t value);                          // 5 bits
