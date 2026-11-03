// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "models.h"

// IS31FL3741: the LED Glasses' and the 13x9 RGB matrix's driver. 0xfd picks a page (0 and 1 the
// 351 LEDs' brightness, 2 and 3 their scaling, 4 the configuration), 0xfe unlocks it, 0xfc reads
// the address doubled as the chip's id; the other registers are the page's, pointed at and
// filled byte by byte.

#define PAGES 5

// Where page p's registers are kept: the LED pages in gram, the configuration in regs.
static uint8_t *_page_reg(i2c_virtual_device_t *dev, uint8_t page, uint8_t reg) {
    static const uint16_t start[4] = { 0, 180, 512, 692 }, size[4] = { 180, 171, 180, 171 };
    if (page < 4) {
        return reg < size[page] ? &dev->gram[start[page] + reg] : NULL;
    }
    return page == 4 ? &dev->regs[reg] : NULL;
}

static void _is31fl3741_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    uint8_t reg = data[0];
    dev->pointer = reg;
    if (reg >= 0xfc) {
        if (len >= 2 && reg == 0xfd && data[1] < PAGES) {
            dev->page = data[1];
        }
        return;
    }
    for (size_t i = 1; i < len; i++, reg++) {
        uint8_t *at = _page_reg(dev, dev->page, reg);
        if (at != NULL) {
            *at = data[i];
        }
    }
}

static void _is31fl3741_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t reg = (uint8_t)(dev->pointer + i);
        uint8_t *at = _page_reg(dev, dev->page, reg);
        data[i] = reg == 0xfc ? (uint8_t)(dev->address * 2) : reg == 0xfd ? dev->page : at != NULL ? *at : 0;
    }
}

const i2c_virtual_model_t i2c_model_is31fl3741 = {
    "is31fl3741", 0x30, 0, { { NULL } }, _is31fl3741_write, _is31fl3741_read, .driver = "adafruit_is31fl3741"
};
