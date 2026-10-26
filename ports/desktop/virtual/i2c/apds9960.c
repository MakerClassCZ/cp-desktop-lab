// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "models.h"

// APDS9960: proximity and colour, 16-bit little-endian counts; no gesture comes

static void _apds9960_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    uint16_t r = (uint16_t)dev->value[1], g = (uint16_t)dev->value[2], b = (uint16_t)dev->value[3];
    uint16_t colour[4] = { (uint16_t)(r + g + b), r, g, b };    // clear, red, green, blue
    for (size_t i = 0; i < len; i++) {
        uint8_t reg = dev->pointer++;
        if (reg == 0x92) {
            data[i] = 0xab;
        } else if (reg == 0x93) {
            data[i] = 0x03;             // colour and proximity valid
        } else if (reg >= 0x94 && reg <= 0x9b) {
            uint16_t c = colour[(reg - 0x94) / 2];
            data[i] = (reg & 1) ? (uint8_t)(c >> 8) : (uint8_t)c;
        } else if (reg == 0x9c) {
            data[i] = (uint8_t)dev->value[0];
        } else if (reg == 0xae || reg == 0xaf) {
            data[i] = 0;                // the gesture FIFO empty
        } else {
            data[i] = dev->regs[reg];
        }
    }
}

const i2c_virtual_model_t i2c_model_apds9960 = {
    "apds9960", 0x39, 4, {
        { "prox", "", 0, 255, 0, 0 }, { "red", "", 0, 4096, 300, 0 }, { "green", "", 0, 4096, 300, 0 },
        { "blue", "", 0, 4096, 300, 0 }
    },
    i2c_model_plain_write, _apds9960_read
};
