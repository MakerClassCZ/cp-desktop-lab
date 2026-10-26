// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>

#include "models.h"

// MCP9808: a thermometer with 16-bit registers, most significant byte first

static void _mcp9808_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    dev->pointer = data[0] & 0x0f;
    if (len == 2) {
        dev->regs[dev->pointer] = data[1];
    } else if (len >= 3) {
        dev->words[dev->pointer] = (uint16_t)(data[1] << 8 | data[2]);
    }
}

static void _mcp9808_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    uint16_t word;
    switch (dev->pointer) {
        case 0x05: {
            int counts = (int)lround(dev->value[0] * 16);
            word = (uint16_t)(counts & 0x1fff);     // 13 bits, two's complement
            break;
        }
        case 0x06:
            word = 0x0054;
            break;
        case 0x07:
            word = 0x0400;
            break;
        case 0x08:
            word = (uint16_t)(dev->regs[0x08] << 8);    // the resolution: one byte
            break;
        default:
            word = dev->words[dev->pointer];
    }
    for (size_t i = 0; i < len; i++) {
        data[i] = i % 2 == 0 ? (uint8_t)(word >> 8) : (uint8_t)word;
    }
}

const i2c_virtual_model_t i2c_model_mcp9808 = {
    "mcp9808", 0x18, 1, { { "temp", "C", -40, 125, 22, 2 } }, _mcp9808_write, _mcp9808_read
};
