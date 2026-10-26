// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "vdisplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "panel.h"

static vdisplay_t _displays[VDISPLAYS_MAX];
static int _count;

vdisplay_t *vdisplay_add(const char *name, uint16_t width, uint16_t height) {
    if (_count == VDISPLAYS_MAX) {
        return NULL;
    }
    uint16_t *pixels = calloc((size_t)width * height, sizeof(uint16_t));
    if (pixels == NULL) {
        return NULL;
    }
    vdisplay_t *d = &_displays[_count++];
    snprintf(d->name, sizeof(d->name), "%s", name);
    d->width = width;
    d->height = height;
    d->pixels = pixels;
    d->on = false;
    panel_changed();
    return d;
}

int vdisplay_count(void) {
    return _count;
}

vdisplay_t *vdisplay_at(int i) {
    return i >= 0 && i < _count ? &_displays[i] : NULL;
}

vdisplay_t *vdisplay_by_name(const char *name) {
    for (int i = 0; i < _count; i++) {
        if (strcasecmp(_displays[i].name, name) == 0) {
            return &_displays[i];
        }
    }
    return NULL;
}

bool vdisplay_resize(vdisplay_t *d, uint16_t width, uint16_t height) {
    if (d->width == width && d->height == height) {
        return true;
    }
    uint16_t *pixels = calloc((size_t)width * height, sizeof(uint16_t));
    if (pixels == NULL) {
        return false;
    }
    free(d->pixels);
    d->pixels = pixels;
    d->width = width;
    d->height = height;
    panel_changed();
    return true;
}

void vdisplay_changed(vdisplay_t *display) {
    display->dirty = true;
    panel_changed();
}

bool vdisplay_dump(const vdisplay_t *d, const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)d->width, (unsigned)d->height);
    for (size_t i = 0; i < (size_t)d->width * d->height; i++) {
        uint16_t c = d->on ? d->pixels[i] : 0;
        uint8_t rgb[3] = {
            (uint8_t)(((c >> 11) & 0x1f) * 255 / 31),
            (uint8_t)(((c >> 5) & 0x3f) * 255 / 63),
            (uint8_t)((c & 0x1f) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f) == 0;
}
