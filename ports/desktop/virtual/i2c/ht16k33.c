// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "models.h"

// HT16K33: the LED matrix and segment backpacks' driver. A byte under 0x10 points into its 16
// bytes of display memory, the bytes after it fill it; other bytes are commands (the
// oscillator 0x2x, the display and its blinking 0x8x, the brightness 0xex), kept as they come.

static void _ht16k33_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    if (data[0] >= 0x10) {
        dev->regs[0x10 + (data[0] >> 4)] = data[0];
        return;
    }
    for (size_t i = 1; i < len; i++) {
        dev->regs[(data[0] + i - 1) & 0x0f] = data[i];
    }
    dev->pointer = data[0];
}

static void _ht16k33_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        data[i] = dev->regs[(dev->pointer + i) & 0x0f];
    }
}

const i2c_virtual_model_t i2c_model_ht16k33 = {
    "ht16k33", 0x70, 0, { { NULL } }, _ht16k33_write, _ht16k33_read, .driver = "adafruit_ht16k33"
};
