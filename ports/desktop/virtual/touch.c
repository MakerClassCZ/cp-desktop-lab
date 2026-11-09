// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "touch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bus_trace.h"
#include "desktop_pins.h"
#include "panel.h"
#include "sdl_host.h"
#include "vdisplay.h"

#define SURFACES_MAX 4
#define DISPLAYS_MAX (VDISPLAYS_MAX + 1)

// How a touch panel sits on its display's glass, beyond the controller's own frame.
#define FLIP_X 1
#define FLIP_Y 2
#define SWAP 4

typedef struct {
    char name[16];
    char display[16];           // "" = the default display
    uint8_t pins[4];            // X1 X2 Y1 Y2, adafruit_touchscreen's order
    uint16_t calibration[4];    // the raw readings at the edges: x min, x max, y min, y max
    uint8_t flags;
} resistive_t;

typedef struct {
    char display[16];
    bool down;
    int x, y;                   // in what the display shows
} display_touch_t;

static resistive_t _resistive[SURFACES_MAX];
static int _resistive_count;
static display_touch_t _touches[DISPLAYS_MAX];
static int _touch_count;

// The display "" stands for: the board file's first, else the main window.
static const char *_display_name(const char *display) {
    if (display != NULL && display[0] != '\0') {
        return display;
    }
    return vdisplay_count() > 0 ? vdisplay_at(0)->name : TOUCH_MAIN_DISPLAY;
}

static bool _same(const char *bound, const char *display) {
    return strcasecmp(_display_name(bound), display) == 0;
}

static uint8_t _flags(const char *options) {
    uint8_t flags = 0;
    char copy[96];
    snprintf(copy, sizeof(copy), "%s", options != NULL ? options : "");
    for (char *word = strtok(copy, " "); word != NULL; word = strtok(NULL, " ")) {
        flags |= strcmp(word, "swap") == 0 ? SWAP : strcmp(word, "flip_x") == 0 ? FLIP_X : strcmp(word, "flip_y") == 0 ? FLIP_Y : 0;
    }
    return flags;
}

static void _display_option(const char *options, char *out, size_t size) {
    out[0] = '\0';
    const char *d = options != NULL ? strstr(options, "display=") : NULL;
    if (d != NULL) {
        size_t n = strcspn(d + 8, " ");
        snprintf(out, size, "%.*s", (int)(n < size ? n : size - 1), d + 8);
    }
}

bool touch_add_resistive(const char *name, const uint8_t pins[4], const char *options, const char **why) {
    resistive_t *r = NULL;
    for (int i = 0; i < _resistive_count; i++) {
        if (strcasecmp(_resistive[i].name, name) == 0) {
            r = &_resistive[i];
        }
    }
    if (r == NULL && _resistive_count == SURFACES_MAX) {
        *why = "more than 4 touchscreens";
        return false;
    }
    if (strlen(name) >= sizeof(r->name)) {
        *why = "a name of 15 characters at most";
        return false;
    }
    resistive_t made = { .calibration = { 0, 65535, 0, 65535 } };
    snprintf(made.name, sizeof(made.name), "%s", name);
    memcpy(made.pins, pins, 4);
    _display_option(options, made.display, sizeof(made.display));
    made.flags = _flags(options);
    const char *c = options != NULL ? strstr(options, "calibration=") : NULL;
    if (c != NULL) {
        unsigned v[4];
        if (sscanf(c + 12, "%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3]) != 4
            || v[0] > 65535 || v[1] > 65535 || v[2] > 65535 || v[3] > 65535 || v[0] == v[1] || v[2] == v[3]) {
            *why = "calibration=XMIN,XMAX,YMIN,YMAX, raw readings 0-65535";
            return false;
        }
        for (int i = 0; i < 4; i++) {
            made.calibration[i] = (uint16_t)v[i];
        }
    }
    if (r == NULL) {
        r = &_resistive[_resistive_count++];
    }
    *r = made;
    return true;
}

void touch_bind_i2c(i2c_virtual_device_t *dev, const char *display, const char *options) {
    snprintf(dev->touch_display, sizeof(dev->touch_display), "%s", display != NULL ? display : "");
    dev->touch_flags = _flags(options);
}

static display_touch_t *_touch(const char *display, bool make) {
    for (int i = 0; i < _touch_count; i++) {
        if (strcasecmp(_touches[i].display, display) == 0) {
            return &_touches[i];
        }
    }
    if (!make || _touch_count == DISPLAYS_MAX) {
        return NULL;
    }
    display_touch_t *t = &_touches[_touch_count++];
    memset(t, 0, sizeof(*t));
    snprintf(t->display, sizeof(t->display), "%s", display);
    return t;
}

bool touch_display_bound(const char *display) {
    for (int i = 0; i < _resistive_count; i++) {
        if (_same(_resistive[i].display, display)) {
            return true;
        }
    }
    for (int i = 0; i2c_virtual_device(i) != NULL; i++) {
        i2c_virtual_device_t *dev = i2c_virtual_device(i);
        if (dev->present && dev->model->touch != I2C_TOUCH_NONE && _same(dev->touch_display, display)) {
            return true;
        }
    }
    return false;
}

// The display's size as it shows and the turn from the controller's memory to that picture.
static bool _geometry(const char *display, int *w, int *h, int *rotation) {
    *rotation = 0;
    if (strcasecmp(display, TOUCH_MAIN_DISPLAY) == 0) {
        uint16_t fw, fh;
        const uint16_t *pixels;
        if (!desktop_sdl_frame(&fw, &fh, &pixels)) {
            return false;
        }
        *w = fw;
        *h = fh;
        return true;
    }
    vdisplay_t *d = vdisplay_by_name(display);
    if (d == NULL) {
        return false;
    }
    *w = d->width;
    *h = d->height;
    *rotation = d->rotation;
    return true;
}

// Where a touch at x, y of the picture falls on the panel, 0-1 across and down its frame: the
// controller's memory (spi_virtual.c turns the picture back by the program's rotation), then
// the panel's own swap and flips. cw, ch: that frame's size in pixels.
static bool _position(const char *display, uint8_t flags, double *fx, double *fy, int *cx, int *cy, int *cw, int *ch) {
    display_touch_t *t = _touch(display, false);
    int w, h, rotation;
    if (t == NULL || !_geometry(display, &w, &h, &rotation)) {
        return false;
    }
    int x = t->x < 0 ? 0 : t->x >= w ? w - 1 : t->x;
    int y = t->y < 0 ? 0 : t->y >= h ? h - 1 : t->y;
    int mx = x, my = y, mw = w, mh = h;
    if (rotation == 180) {
        mx = w - 1 - x;
        my = h - 1 - y;
    } else if (rotation == 90) {
        mx = h - 1 - y;
        my = x;
        mw = h;
        mh = w;
    } else if (rotation == 270) {
        mx = y;
        my = w - 1 - x;
        mw = h;
        mh = w;
    }
    if (flags & SWAP) {
        int s = mx;
        mx = my;
        my = s;
        s = mw;
        mw = mh;
        mh = s;
    }
    if (flags & FLIP_X) {
        mx = mw - 1 - mx;
    }
    if (flags & FLIP_Y) {
        my = mh - 1 - my;
    }
    *cx = mx;
    *cy = my;
    *cw = mw;
    *ch = mh;
    *fx = mw > 1 ? (double)mx / (mw - 1) : 0;
    *fy = mh > 1 ? (double)my / (mh - 1) : 0;
    return true;
}

// The I2C controllers on the display take the touch as their channels: x, y, then the pressure
// (a raw 12-bit controller, 1000 while down) or touched (1 while down).
static void _update_i2c(const char *display, bool down) {
    for (int i = 0; i2c_virtual_device(i) != NULL; i++) {
        i2c_virtual_device_t *dev = i2c_virtual_device(i);
        if (!dev->present || dev->model->touch == I2C_TOUCH_NONE || !_same(dev->touch_display, display)) {
            continue;
        }
        double fx, fy;
        int cx, cy, cw, ch;
        if (!_position(display, dev->touch_flags, &fx, &fy, &cx, &cy, &cw, &ch)) {
            continue;
        }
        if (dev->model->touch == I2C_TOUCH_RAW12) {
            dev->value[0] = fx * 4095;
            dev->value[1] = fy * 4095;
            dev->value[2] = down ? 1000 : 0;
        } else {
            dev->value[0] = cx;
            dev->value[1] = cy;
            dev->value[2] = down ? 1 : 0;
        }
    }
    panel_changed();
}

bool touch_set(const char *display, bool down, int x, int y) {
    int w, h, rotation;
    if (!touch_display_bound(display) || !_geometry(display, &w, &h, &rotation)) {
        return false;
    }
    display_touch_t *t = _touch(display, true);
    if (t == NULL) {
        return false;
    }
    bool moved = down && (!t->down || t->x != x || t->y != y);
    bool lifted = !down && t->down;
    t->down = down;
    if (down) {
        t->x = x;
        t->y = y;
    }
    if (moved) {
        bus_trace_text("TOUCHSCREEN %s %d,%d", display, x, y);
    } else if (lifted) {
        bus_trace_text("TOUCHSCREEN %s up", display);
    }
    _update_i2c(display, down);
    return true;
}

bool touch_state(const char *display, bool *down, int *x, int *y) {
    if (!touch_display_bound(display)) {
        return false;
    }
    display_touch_t *t = _touch(display, false);
    *down = t != NULL && t->down;
    *x = t != NULL ? t->x : 0;
    *y = t != NULL ? t->y : 0;
    return true;
}

// ---- the resistive panel ----

static resistive_t *_owner(uint8_t pin, int *index) {
    for (int i = 0; i < _resistive_count; i++) {
        for (int k = 0; k < 4; k++) {
            if (_resistive[i].pins[k] == pin) {
                *index = k;
                return &_resistive[i];
            }
        }
    }
    return NULL;
}

bool touch_owns_pin(uint8_t pin) {
    int k;
    return desktop_pin_backend == &desktop_virtual_backend && _owner(pin, &k) != NULL;
}

// A pin's drive: 1 = high, 0 = low, -1 = an input (open).
static int _drive(uint8_t pin) {
    bool output, level;
    if (!desktop_virtual_pin(pin, &output, &level) || !output) {
        return -1;
    }
    return level ? 1 : 0;
}

// Two plates, each its two pins' ends joined through the plate: an electrode's resistance at each
// end and the plate's share by where the touch is. The plate on Y1-Y2 carries the display's x
// (adafruit_touchscreen drives Y1-Y2 to read x), X1-X2 its y. Touched, the plates' contact points
// meet through the touch's resistance. Outputs are ideal sources, inputs draw nothing.
#define R_PLATE 300.0
#define R_ELECTRODE 50.0
#define R_TOUCH 100.0
#define V_FULL 65535.0

uint16_t touch_pin_value(uint8_t pin) {
    int k;
    resistive_t *r = _owner(pin, &k);
    if (r == NULL) {
        return 0;
    }
    int own = _drive(pin);
    if (own >= 0) {
        return own ? 65535 : 0;
    }
    const char *display = _display_name(r->display);
    display_touch_t *t = _touch(display, false);
    double fx = 0, fy = 0;
    int cx, cy, cw, ch;
    bool touched = t != NULL && t->down && _position(display, r->flags, &fx, &fy, &cx, &cy, &cw, &ch);
    // Raw readings over the display's edges as the calibration has them.
    double rx = (r->calibration[0] + fx * (r->calibration[1] - r->calibration[0])) / V_FULL;
    double ry = (r->calibration[2] + fy * (r->calibration[3] - r->calibration[2])) / V_FULL;
    // Plate 0: X1-X2 (contact at ry from X1); plate 1: Y1-Y2 (contact at rx from Y1).
    int plate = k < 2 ? 0 : 1;
    double where[2] = { ry, rx };
    int ends[2][2] = { { _drive(r->pins[0]), _drive(r->pins[1]) }, { _drive(r->pins[2]), _drive(r->pins[3]) } };
    // Each plate: its driven ends (voltage and resistance to the contact).
    double v[2][2], res[2][2];
    int n[2] = { 0, 0 };
    for (int p = 0; p < 2; p++) {
        for (int e = 0; e < 2; e++) {
            if (ends[p][e] >= 0) {
                v[p][n[p]] = ends[p][e] * V_FULL;
                res[p][n[p]] = R_ELECTRODE + R_PLATE * (e == 0 ? where[p] : 1 - where[p]);
                n[p]++;
            }
        }
    }
    int other = 1 - plate;
    // A plate driven at both ends is a divider: its contact's voltage, whatever touches it (the
    // electrodes left out, so the reading is the calibration's).
    double divider[2];
    for (int p = 0; p < 2; p++) {
        divider[p] = n[p] == 2 ? v[p][0] + (v[p][1] - v[p][0]) * where[p] : 0;
    }
    double value;
    if (!touched) {
        value = n[plate] == 0 ? 0 : n[plate] == 1 ? v[plate][0] : divider[plate];
    } else if (n[plate] == 0) {
        // Nothing drives this plate: it reads the other's contact.
        value = n[other] == 0 ? 0 : n[other] == 1 ? v[other][0] : divider[other];
    } else if (n[plate] == 2) {
        value = divider[plate];
    } else if (n[other] == 0) {
        value = v[plate][0];
    } else {
        // One end of this plate driven, the other plate's contact a source through its own
        // resistance: a divider along this plate, the touch and the other plate.
        double source = n[other] == 1 ? v[other][0] : divider[other];
        double beyond = R_TOUCH + (n[other] == 1 ? res[other][0] : res[other][0] * res[other][1] / (res[other][0] + res[other][1]));
        value = v[plate][0] + (source - v[plate][0]) * res[plate][0] / (res[plate][0] + beyond);
    }
    return (uint16_t)(value < 0 ? 0 : value > V_FULL ? V_FULL : value + 0.5);
}
