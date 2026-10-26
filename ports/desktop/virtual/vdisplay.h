// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

// The displays a board file puts on the virtual buses (spi_virtual.c, i2c_virtual.c): what each
// shows, as the panel draws it, upright, RGB565.

typedef struct {
    char name[16];
    uint16_t width, height;
    uint16_t *pixels;           // width * height
    bool on;                    // the display was told to show its memory
    bool dirty;                 // pixels changed since the panel last uploaded them
} vdisplay_t;

#define VDISPLAYS_MAX 4

vdisplay_t *vdisplay_add(const char *name, uint16_t width, uint16_t height);
int vdisplay_count(void);
vdisplay_t *vdisplay_at(int i);
vdisplay_t *vdisplay_by_name(const char *name);
// A new size, the memory cleared (an RGB matrix of another size); false when out of memory.
bool vdisplay_resize(vdisplay_t *display, uint16_t width, uint16_t height);
// A pixel changed: the panel draws again.
void vdisplay_changed(vdisplay_t *display);
// What the display shows, as a binary PPM; false when the file cannot be written.
bool vdisplay_dump(const vdisplay_t *display, const char *path);
