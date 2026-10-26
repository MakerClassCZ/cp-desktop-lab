// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "models.h"

// SSD1306: a monochrome OLED; each control value says whether commands or data follow

// The parameters each command takes; the rest take none.
static uint8_t _ssd1306_params(uint8_t command) {
    switch (command) {
        case 0x20:
        case 0x81:
        case 0x8d:
        case 0xa8:
        case 0xad:
        case 0xd3:
        case 0xd5:
        case 0xd9:
        case 0xda:
        case 0xdb:
            return 1;
        case 0x21:
        case 0x22:
        case 0xa3:
            return 2;
        case 0x29:
        case 0x2a:
            return 5;
        case 0x26:
        case 0x27:
            return 6;
        default:
            return 0;
    }
}

// A memory value onto the picture: eight pixels down from its page's top, lit or dark.
static void _ssd1306_show(i2c_virtual_device_t *dev, int col, int page) {
    vdisplay_t *d = dev->display;
    uint8_t value = dev->gram[page * 128 + col];
    for (int bit = 0; bit < 8; bit++) {
        int y = page * 8 + bit;
        if (col < d->width && y < d->height) {
            bool lit = ((value >> bit) & 1) != dev->inverted;
            d->pixels[y * d->width + col] = lit ? 0xffff : 0x0000;
        }
    }
}

static void _ssd1306_redraw(i2c_virtual_device_t *dev) {
    for (int page = 0; page < 8; page++) {
        for (int col = 0; col < 128; col++) {
            _ssd1306_show(dev, col, page);
        }
    }
    vdisplay_changed(dev->display);
}

static void _ssd1306_command(i2c_virtual_device_t *dev, uint8_t value) {
    if (dev->params_left > 0) {
        dev->params[_ssd1306_params(dev->command) - dev->params_left--] = value;
        if (dev->params_left == 0) {
            switch (dev->command) {
                case 0x20:
                    dev->mode = dev->params[0] & 3;
                    break;
                case 0x21:
                    dev->col0 = dev->col = dev->params[0] & 0x7f;
                    dev->col1 = dev->params[1] & 0x7f;
                    break;
                case 0x22:
                    dev->page0 = dev->page = dev->params[0] & 7;
                    dev->page1 = dev->params[1] & 7;
                    break;
            }
        }
        return;
    }
    dev->command = value;
    dev->params_left = _ssd1306_params(value);
    if (value == 0xae || value == 0xaf) {
        dev->display->on = value == 0xaf;
        vdisplay_changed(dev->display);
    } else if (value == 0xa6 || value == 0xa7) {
        dev->inverted = value == 0xa7;
        _ssd1306_redraw(dev);
    } else if (value >= 0xb0 && value <= 0xb7) {
        dev->page = value & 7;       // page addressing
    } else if (value <= 0x0f) {
        dev->col = (dev->col & 0xf0) | value;
    } else if (value >= 0x10 && value <= 0x1f) {
        dev->col = (uint8_t)((dev->col & 0x0f) | ((value & 0x0f) << 4));
    }
}

static void _ssd1306_data(i2c_virtual_device_t *dev, uint8_t value) {
    dev->gram[dev->page * 128 + (dev->col & 0x7f)] = value;
    _ssd1306_show(dev, dev->col & 0x7f, dev->page);
    if (dev->mode == 2) {           // page addressing: along the page only
        dev->col = (dev->col + 1) & 0x7f;
        return;
    }
    if (dev->mode == 1) {           // vertical: down the pages, then the next column
        if (++dev->page > dev->page1) {
            dev->page = dev->page0;
            dev->col = dev->col >= dev->col1 ? dev->col0 : dev->col + 1;
        }
        return;
    }
    if (++dev->col > dev->col1) {   // horizontal
        dev->col = dev->col0;
        dev->page = dev->page >= dev->page1 ? dev->page0 : dev->page + 1;
    }
}

static void _ssd1306_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    // A control value: bit 6 data rather than commands; bit 7 one value, then another control value.
    size_t i = 0;
    while (i < len) {
        uint8_t control = data[i++];
        size_t end = (control & 0x80) ? (i < len ? i + 1 : i) : len;
        for (; i < end; i++) {
            if (control & 0x40) {
                _ssd1306_data(dev, data[i]);
            } else {
                _ssd1306_command(dev, data[i]);
            }
        }
    }
    vdisplay_changed(dev->display);
}

static void _ssd1306_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    // The status value: display on or off.
    memset(data, dev->display->on ? 0x03 : 0x43, len);
}

static bool _ssd1306_attach(i2c_virtual_device_t *dev, unsigned width, unsigned height, const char **why) {
    width = width ? width : 128;
    height = height ? height : 64;
    if (width > 128 || height > 64 || height % 8 != 0) {
        *why = "an SSD1306 is at most 128x64, its height a multiple of 8";
        return false;
    }
    dev->display = vdisplay_add(dev->name, (uint16_t)width, (uint16_t)height);
    if (dev->display == NULL) {
        *why = "too many displays";
        return false;
    }
    dev->col1 = 127;
    dev->page1 = 7;
    return true;
}

const i2c_virtual_model_t i2c_model_ssd1306 = {
    "ssd1306", 0x3c, 0, { { NULL } }, _ssd1306_write, _ssd1306_read, _ssd1306_attach
};
