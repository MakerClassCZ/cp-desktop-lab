// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "epaper_virtual.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "py/obj.h"
#include "shared-bindings/digitalio/DigitalInOut.h"
#include "shared-bindings/fourwire/FourWire.h"
#include "shared-module/fourwire/FourWire.h"

#include "bus_trace.h"
#include "desktop_pins.h"
#include "desktop_port.h"
#include "vdisplay.h"

#define PANELS_MAX 2
#define DEFAULT_REFRESH_MS 100

typedef struct {
    bool used, attached;
    char name[16];
    int cs, dc, busy;
    uint32_t refresh_ms;
    bool stuck;
    // As the driver declared the panel.
    uint16_t width, height, rotation, ram_width, ram_height;
    int16_t colstart, rowstart;
    uint16_t col_cmd, row_cmd, cur_col_cmd, cur_row_cmd, black_cmd, color_cmd;
    int refresh_cmd;
    bool busy_state, little_endian, black_inv, color_inv, grayscale, tricolor, acep, spectra6;
    uint32_t highlight;
    uint8_t depth;
    // The controller.
    uint8_t *planes[2];
    size_t row_bytes;
    int plane;                  // the RAM being written: 0 black, 1 colour, -1 none
    uint8_t command;
    uint8_t params[4];
    int param_count;
    uint16_t wx1, wx2, wy1, wy2, x, y;
    bool busy_now;
    uint64_t busy_until_ns;
    vdisplay_t *display;
} panel_t;

static panel_t _panels[PANELS_MAX];

static panel_t *_by_cs(int cs) {
    for (int i = 0; i < PANELS_MAX; i++) {
        if (_panels[i].used && _panels[i].cs == cs) {
            return &_panels[i];
        }
    }
    return NULL;
}

static panel_t *_new(int cs) {
    for (int i = 0; i < PANELS_MAX; i++) {
        if (!_panels[i].used) {
            panel_t *p = &_panels[i];
            memset(p, 0, sizeof(*p));
            p->used = true;
            p->cs = cs;
            p->dc = p->busy = -1;
            p->refresh_ms = DEFAULT_REFRESH_MS;
            return p;
        }
    }
    return NULL;
}

bool epaper_virtual_declare(const char *name, const char *spec, int cs, const char **why) {
    if (cs < 0) {
        *why = "an e-paper panel needs its cs= pin";
        return false;
    }
    panel_t *p = _by_cs(cs) != NULL ? _by_cs(cs) : _new(cs);
    if (p == NULL) {
        *why = "more than 2 e-paper panels";
        return false;
    }
    snprintf(p->name, sizeof(p->name), "%s", name);
    char copy[128];
    snprintf(copy, sizeof(copy), "%s", spec);
    for (char *word = strtok(copy, " "); word != NULL; word = strtok(NULL, " ")) {
        unsigned ms;
        if (strcasecmp(word, "epaper") == 0) {
            continue;
        } else if (strcasecmp(word, "stuck") == 0) {
            p->stuck = true;
        } else if (sscanf(word, "refresh_ms=%u", &ms) == 1) {
            p->refresh_ms = ms;
        } else {
            static char message[64];
            snprintf(message, sizeof(message), "e-paper: unknown option %.32s", word);
            *why = message;
            p->used = false;
            return false;
        }
    }
    return true;
}

bool epaper_virtual_uses_pin(int pin) {
    for (int i = 0; i < PANELS_MAX; i++) {
        if (_panels[i].used && (_panels[i].cs == pin || _panels[i].dc == pin)) {
            return true;
        }
    }
    return false;
}

static int _pin_of(mp_obj_t io) {
    if (io == MP_OBJ_NULL || !mp_obj_is_type(io, &digitalio_digitalinout_type)) {
        return -1;
    }
    const digitalio_digitalinout_obj_t *d = MP_OBJ_TO_PTR(io);
    return d->pin != NULL ? d->pin->number : -1;
}

static void _set_busy(panel_t *p, bool busy) {
    p->busy_now = busy;
    if (p->busy >= 0) {
        desktop_pin_drive((uint8_t)p->busy, busy ? p->busy_state : !p->busy_state);
    }
    if (p->display != NULL) {
        p->display->busy = busy;
    }
}

void epaperdisplay_port_attach(epaperdisplay_epaperdisplay_obj_t *self, const epaperdisplay_construct_args_t *args) {
    (void)self;
    if (desktop_pin_backend != &desktop_virtual_backend || !mp_obj_is_type(args->bus, &fourwire_fourwire_type)) {
        return;
    }
    const fourwire_fourwire_obj_t *bus = MP_OBJ_TO_PTR(args->bus);
    int cs = _pin_of(bus->chip_select);
    if (cs < 0) {
        return;
    }
    panel_t *p = _by_cs(cs);
    if (p == NULL && (p = _new(cs)) == NULL) {
        return;
    }
    if (p->name[0] == '\0') {
        snprintf(p->name, sizeof(p->name), "EPD%s", p == &_panels[0] ? "" : "2");
    }
    p->dc = _pin_of(bus->command);
    p->busy = args->busy_pin != NULL ? args->busy_pin->number : -1;
    p->busy_state = args->busy_state;
    p->width = args->width;
    p->height = args->height;
    p->rotation = args->rotation % 360;
    p->ram_width = args->ram_width;
    p->ram_height = args->ram_height;
    p->colstart = args->colstart;
    p->rowstart = args->rowstart;
    p->col_cmd = args->set_column_window_command;
    p->row_cmd = args->set_row_window_command;
    p->cur_col_cmd = args->set_current_column_command;
    p->cur_row_cmd = args->set_current_row_command;
    p->black_cmd = args->write_black_ram_command;
    p->color_cmd = args->write_color_ram_command;
    p->refresh_cmd = args->refresh_sequence_len > 0 ? args->refresh_sequence[0] : -1;
    p->little_endian = args->address_little_endian;
    p->black_inv = args->black_bits_inverted;
    p->color_inv = args->color_bits_inverted;
    p->acep = args->acep;
    p->spectra6 = args->spectra6;
    p->depth = (args->acep || args->spectra6) ? 4 : 1;
    p->grayscale = args->grayscale && p->depth == 1 && args->highlight_color == 0;
    p->tricolor = args->highlight_color != 0 && p->depth == 1;
    p->highlight = args->highlight_color;
    p->row_bytes = ((size_t)p->ram_width * p->depth + 7) / 8;
    for (int i = 0; i < 2; i++) {
        free(p->planes[i]);
        // Blank RAM shows white: light black bits, no colour, the white index.
        uint8_t blank = p->depth == 4 ? 0x11 : i == 0 ? (p->black_inv ? 0x00 : 0xff) : (p->color_inv ? 0xff : 0x00);
        p->planes[i] = malloc(p->row_bytes * p->ram_height);
        if (p->planes[i] != NULL) {
            memset(p->planes[i], blank, p->row_bytes * p->ram_height);
        }
    }
    p->plane = -1;
    // Without window commands the data fills the panel row after row, as its resolution setting
    // says: the display's size in the controller's orientation.
    bool turned = p->rotation == 90 || p->rotation == 270;
    p->wx1 = p->wy1 = 0;
    p->wx2 = (uint16_t)(((turned ? p->height : p->width) * p->depth + 7) / 8 - 1);
    p->wy2 = (uint16_t)((turned ? p->width : p->height) - 1);
    uint16_t w = p->width, h = p->height;
    if (p->display == NULL) {
        p->display = vdisplay_by_name(p->name);
    }
    if (p->display == NULL) {
        p->display = vdisplay_add(p->name, w, h);
    } else if (p->display->width != w || p->display->height != h) {
        vdisplay_resize(p->display, w, h);
    }
    if (p->display != NULL) {
        p->display->epaper = true;
        p->display->on = true;
        // A new panel shows white until its first refresh.
        for (size_t i = 0; i < (size_t)w * h; i++) {
            p->display->pixels[i] = 0xffff;
        }
        vdisplay_changed(p->display);
    }
    p->attached = true;
    _set_busy(p, false);
}

static bool _low(int pin) {
    bool output, level;
    return pin >= 0 && desktop_virtual_pin((uint8_t)pin, &output, &level) && output && !level;
}

static uint16_t _value(const panel_t *p, const uint8_t *b, bool wide) {
    if (!wide) {
        return b[0];
    }
    return p->little_endian ? (uint16_t)(b[0] | b[1] << 8) : (uint16_t)(b[0] << 8 | b[1]);
}

static int _bit(const panel_t *p, int plane, int px, int py) {
    if (p->planes[plane] == NULL || px < 0 || py < 0 || px >= p->ram_width || py >= p->ram_height) {
        return -1;
    }
    uint8_t cell = p->planes[plane][(size_t)py * p->row_bytes + (size_t)(px * p->depth) / 8];
    if (p->depth == 4) {
        return px % 2 == 0 ? cell >> 4 : cell & 0x0f;
    }
    return (cell >> (7 - px % 8)) & 1;
}

static uint16_t _rgb565(uint32_t rgb) {
    return (uint16_t)((rgb >> 8 & 0xf800) | (rgb >> 5 & 0x07e0) | (rgb >> 3 & 0x001f));
}

static uint32_t _colour(const panel_t *p, int px, int py) {
    static const uint32_t acep[8] = {
        0x000000, 0xffffff, 0x00ff00, 0x0000ff, 0xff0000, 0xffff00, 0xff8000, 0xffffff
    };
    static const uint32_t spectra[8] = {
        0x000000, 0xffffff, 0xffff00, 0xff0000, 0xffffff, 0x0000ff, 0x00ff00, 0xffffff
    };
    int black = _bit(p, 0, px, py);
    if (black < 0) {
        return 0xffffff;
    }
    if (p->depth == 4) {
        return (p->spectra6 ? spectra : acep)[black & 7];
    }
    // The driver's inversions undone: displayio's own bits, 1 light.
    black ^= p->black_inv;
    int colour = _bit(p, 1, px, py);
    colour = colour < 0 ? 0 : colour ^ p->color_inv;
    if (p->grayscale) {
        static const uint32_t grey[4] = { 0x000000, 0x555555, 0xaaaaaa, 0xffffff };
        return grey[black << 1 | colour];
    }
    if (p->tricolor && colour) {
        return p->highlight;
    }
    return black ? 0xffffff : 0x000000;
}

// The refresh: the glass shows the RAM, turned upright by the program's rotation.
static void _show(panel_t *p) {
    vdisplay_t *d = p->display;
    if (d == NULL) {
        return;
    }
    int w = d->width, h = d->height;
    bool turned = p->rotation == 90 || p->rotation == 270;
    int mw = turned ? h : w, mh = turned ? w : h;
    for (int vy = 0; vy < mh; vy++) {
        for (int vx = 0; vx < mw; vx++) {
            int px = vx, py = vy;
            if (p->rotation == 180) {
                px = mw - 1 - vx;
                py = mh - 1 - vy;
            } else if (p->rotation == 90) {
                px = vy;
                py = mw - 1 - vx;
            } else if (p->rotation == 270) {
                px = mh - 1 - vy;
                py = vx;
            }
            d->pixels[py * w + px] = _rgb565(_colour(p, p->colstart + vx, p->rowstart + vy));
        }
    }
    d->refreshes++;
    vdisplay_changed(d);
}

static void _command(panel_t *p, uint8_t command) {
    p->command = command;
    p->param_count = 0;
    if (command == p->black_cmd || command == p->color_cmd) {
        p->plane = command == p->black_cmd ? 0 : 1;
        if (p->cur_col_cmd == NO_COMMAND) {
            p->x = p->wx1;
        }
        if (p->cur_row_cmd == NO_COMMAND) {
            p->y = p->wy1;
        }
    } else {
        p->plane = -1;
    }
    if ((int)command == p->refresh_cmd) {
        if (p->busy_now) {
            bus_trace_text("EPD %s refresh while busy", p->name);
        }
        _show(p);
        bus_trace_text("EPD %s refresh %lu", p->name, (unsigned long)p->display->refreshes);
        _set_busy(p, true);
        p->busy_until_ns = desktop_program_ns() + (uint64_t)p->refresh_ms * 1000000;
    }
}

static void _data(panel_t *p, uint8_t value) {
    if (p->plane >= 0) {
        if (p->planes[p->plane] != NULL && p->x < p->row_bytes && p->y < p->ram_height) {
            p->planes[p->plane][(size_t)p->y * p->row_bytes + p->x] = value;
        }
        if (++p->x > p->wx2) {
            p->x = p->wx1;
            if (++p->y > p->wy2) {
                p->y = p->wy1;
            }
        }
        return;
    }
    if (p->param_count < 4) {
        p->params[p->param_count++] = value;
    }
    bool wide_x = p->ram_width >= 0x100, wide_y = p->ram_height >= 0x100;
    int n = p->param_count;
    if (p->command == p->col_cmd && n == (wide_x ? 4 : 2)) {
        p->wx1 = _value(p, p->params, wide_x);
        p->wx2 = _value(p, p->params + (wide_x ? 2 : 1), wide_x);
        p->x = p->wx1;
    } else if (p->command == p->row_cmd && n == (wide_y ? 4 : 2)) {
        p->wy1 = _value(p, p->params, wide_y);
        p->wy2 = _value(p, p->params + (wide_y ? 2 : 1), wide_y);
        p->y = p->wy1;
    } else if (p->command == p->cur_col_cmd && n == (wide_x ? 2 : 1)) {
        p->x = _value(p, p->params, wide_x);
    } else if (p->command == p->cur_row_cmd && n == (wide_y ? 2 : 1)) {
        p->y = _value(p, p->params, wide_y);
    }
}

bool epaper_virtual_transfer(const uint8_t *out, uint8_t fill, size_t len) {
    bool any = false;
    for (int i = 0; i < PANELS_MAX; i++) {
        panel_t *p = &_panels[i];
        if (!p->attached || !_low(p->cs)) {
            continue;
        }
        any = true;
        bool command = _low(p->dc);
        for (size_t k = 0; k < len; k++) {
            uint8_t value = out != NULL ? out[k] : fill;
            if (command) {
                _command(p, value);
            } else {
                _data(p, value);
            }
        }
    }
    return any;
}

void epaper_virtual_task(void) {
    for (int i = 0; i < PANELS_MAX; i++) {
        panel_t *p = &_panels[i];
        if (p->attached && p->busy_now && !p->stuck && desktop_program_ns() >= p->busy_until_ns) {
            _set_busy(p, false);
        }
    }
}

bool epaper_virtual_set(const char *name, const char *attr, const char *value, char *err, size_t errlen) {
    panel_t *p = NULL;
    for (int i = 0; i < PANELS_MAX; i++) {
        if (_panels[i].used && strcasecmp(_panels[i].name, name) == 0) {
            p = &_panels[i];
        }
    }
    if (p == NULL) {
        return false;
    }
    if (strcmp(attr, "busy") != 0 || (strcmp(value, "stuck") != 0 && strcmp(value, "auto") != 0)) {
        snprintf(err, errlen, "an e-paper panel takes: set %s busy stuck|auto", p->name);
        return false;
    }
    p->stuck = value[0] == 's';
    if (p->stuck && p->attached) {
        _set_busy(p, true);
    }
    bus_trace_text("EPD %s busy %s", p->name, value);
    return true;
}
