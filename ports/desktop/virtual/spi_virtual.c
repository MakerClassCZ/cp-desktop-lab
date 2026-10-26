// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "spi_virtual.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "vdisplay.h"

#define DEVICES_MAX 4

// A colour TFT that speaks the MIPI display commands: ST7789, ILI9341, HX8357, GC9A01, ST7735.
// It shows the window addresses as the program sends them, less the offset of its glass in the
// controller's memory: the least window start yet, as the first refresh covers the whole glass.
// That is the picture a program sees on the real module, MADCTL's mirroring being what makes
// the module upright.
typedef struct {
    vdisplay_t *display;
    int cs, dc;
    uint8_t command;
    uint8_t params[4];
    int param_count;
    uint16_t x0, x1, y0, y1, x, y;
    uint16_t offset_x, offset_y;        // 0xffff until a window starts
    bool high_byte_pending;
    uint8_t high_byte;
    uint16_t rotation;                  // the program's, which the picture is turned back by
} tft_t;

static tft_t _devices[DEVICES_MAX];
static int _count;

// One MIPI DCS driver answers for each; none with CIRCUITPY_VIRTUAL_DEVICES = 0.
static const char *const _models[] = {
    #if CIRCUITPY_VIRTUAL_DEVICES
    "st7789", "ili9341", "hx8357", "gc9a01", "st7735",
    #endif
    NULL
};

bool spi_virtual_add(const char *name, const char *spec, int cs, int dc, const char **why) {
    static char message[128];
    char model[16] = "";
    unsigned w = 0, h = 0;
    sscanf(spec, "%15s %ux%u", model, &w, &h);
    bool known = false;
    int n = snprintf(message, sizeof(message), "unknown SPI display %s (", model);
    for (size_t i = 0; _models[i] != NULL; i++) {
        known |= strcasecmp(model, _models[i]) == 0;
        if (n < (int)sizeof(message)) {
            const char *sep = i == 0 ? "" : _models[i + 1] == NULL ? " or " : ", ";
            n += snprintf(message + n, sizeof(message) - n, "%s%s", sep, _models[i]);
        }
    }
    if (!known) {
        if (n < (int)sizeof(message)) {
            snprintf(message + n, sizeof(message) - n, "%s)", _models[0] == NULL ? "none in this build" : "");
        }
        *why = message;
        return false;
    }
    if (w == 0 || h == 0 || w > 480 || h > 480) {
        *why = "a display needs its size, such as 240x240";
        return false;
    }
    if (cs < 0 || dc < 0) {
        *why = "a display needs cs= and dc= pins";
        return false;
    }
    if (_count == DEVICES_MAX) {
        *why = "more than 4 SPI devices";
        return false;
    }
    tft_t *t = &_devices[_count];
    memset(t, 0, sizeof(*t));
    t->display = vdisplay_add(name, (uint16_t)w, (uint16_t)h);
    if (t->display == NULL) {
        *why = "too many displays";
        return false;
    }
    t->cs = cs;
    t->dc = dc;
    t->offset_x = t->offset_y = 0xffff;
    _count++;
    return true;
}

vdisplay_t *spi_virtual_display(int cs) {
    for (int i = 0; i < _count; i++) {
        if (_devices[i].cs == cs) {
            return _devices[i].display;
        }
    }
    return NULL;
}

void spi_virtual_set_rotation(int cs, uint16_t rotation) {
    for (int i = 0; i < _count; i++) {
        if (_devices[i].cs == cs) {
            _devices[i].rotation = rotation % 360;
        }
    }
}

static bool _low(int pin) {
    bool output, level;
    return desktop_virtual_pin((uint8_t)pin, &output, &level) && output && !level;
}

static void _command(tft_t *t, uint8_t command) {
    t->command = command;
    t->param_count = 0;
    t->high_byte_pending = false;
    if (command == 0x2c) {
        // RAMWR: the window starts over.
        t->offset_x = t->x0 < t->offset_x ? t->x0 : t->offset_x;
        t->offset_y = t->y0 < t->offset_y ? t->y0 : t->offset_y;
        t->x = t->x0;
        t->y = t->y0;
    } else if (command == 0x01) {     // SWRESET
        t->offset_x = t->offset_y = 0xffff;
    } else if (command == 0x29) {
        t->display->on = true;
        vdisplay_changed(t->display);
    } else if (command == 0x28) {
        t->display->on = false;
        vdisplay_changed(t->display);
    }
}

static void _pixel(tft_t *t, uint16_t colour) {
    int vx = (int)t->x - t->offset_x, vy = (int)t->y - t->offset_y;
    int w = t->display->width, h = t->display->height;
    // The controller's memory is the picture turned: across it is the display's height at 90 and 270.
    int mw = (t->rotation == 90 || t->rotation == 270) ? h : w;
    int mh = (t->rotation == 90 || t->rotation == 270) ? w : h;
    if (vx >= 0 && vy >= 0 && vx < mw && vy < mh) {
        int px = vx, py = vy;
        if (t->rotation == 180) {
            px = mw - 1 - vx;
            py = mh - 1 - vy;
        } else if (t->rotation == 90) {
            px = vy;
            py = mw - 1 - vx;
        } else if (t->rotation == 270) {
            px = mh - 1 - vy;
            py = vx;
        }
        t->display->pixels[py * w + px] = colour;
    }
    if (++t->x > t->x1) {
        t->x = t->x0;
        if (++t->y > t->y1) {
            t->y = t->y0;
        }
    }
}

static void _data(tft_t *t, uint8_t value) {
    switch (t->command) {
        case 0x2a:      // CASET, RASET: start and end, most significant value first
        case 0x2b:
            if (t->param_count < 4) {
                t->params[t->param_count++] = value;
            }
            if (t->param_count == 4) {
                uint16_t start = (uint16_t)(t->params[0] << 8 | t->params[1]);
                uint16_t end = (uint16_t)(t->params[2] << 8 | t->params[3]);
                if (t->command == 0x2a) {
                    t->x0 = start;
                    t->x1 = end;
                } else {
                    t->y0 = start;
                    t->y1 = end;
                }
            }
            break;
        case 0x2c:      // RAMWR: RGB565, most significant value first
            if (!t->high_byte_pending) {
                t->high_byte = value;
                t->high_byte_pending = true;
            } else {
                _pixel(t, (uint16_t)(t->high_byte << 8 | value));
                t->high_byte_pending = false;
            }
            break;
        default:
            break;
    }
}

static bool _construct(bool has_mosi, bool has_miso) {
    (void)has_mosi;
    (void)has_miso;
    return true;
}

static void _deinit(void) {
}

static desktop_spi_result_t _configure(uint32_t hz, uint8_t polarity, uint8_t phase, uint8_t bits, uint32_t *actual_hz) {
    (void)polarity;
    (void)phase;
    if (bits != 8) {
        return DESKTOP_SPI_BAD_BITS;
    }
    *actual_hz = hz;
    return DESKTOP_SPI_OK;
}

static bool _transfer(const uint8_t *out, uint8_t fill, uint8_t *in, size_t len) {
    for (int i = 0; i < _count; i++) {
        tft_t *t = &_devices[i];
        if (!_low(t->cs)) {
            continue;
        }
        bool command = _low(t->dc);
        for (size_t k = 0; k < len; k++) {
            uint8_t value = out != NULL ? out[k] : fill;
            if (command) {
                _command(t, value);
            } else {
                _data(t, value);
            }
        }
        if (t->command == 0x2c) {
            vdisplay_changed(t->display);
        }
    }
    if (in != NULL) {
        memset(in, 0, len);     // the displays send nothing back
    }
    return true;
}

const desktop_spi_ops_t spi_virtual_ops = {
    .sck = DESKTOP_BUS_ANY_PIN,
    .mosi = DESKTOP_BUS_ANY_PIN,
    .miso = DESKTOP_BUS_ANY_PIN,
    .construct = _construct,
    .deinit = _deinit,
    .configure = _configure,
    .transfer = _transfer,
};
