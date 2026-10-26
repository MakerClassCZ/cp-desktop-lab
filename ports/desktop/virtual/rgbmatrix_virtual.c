// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// Protomatter's calls as shared-module/rgbmatrix makes them, for a HUB75 matrix drawn on the
// panel: the display "MATRIX" shows the framebuffer as the program last refreshed it, blank
// while paused (brightness 0). Tiles show as the program laid them out, not as they are wired.

#include <stdlib.h>
#include <string.h>

#include "py/mpconfig.h"

#if CIRCUITPY_RGBMATRIX

#include "lib/protomatter/src/core.h"
#include "supervisor/port.h"
#include "supervisor/shared/tick.h"

#include "vdisplay.h"

Protomatter_core *_PM_protoPtr;

static vdisplay_t *_display;
static bool _ticking;            // the matrix keeps the tick on, which opens and runs the panel

static int _height(const Protomatter_core *core) {
    return core->numRowPairs * 2 * core->parallel * abs(core->tile);
}

ProtomatterStatus _PM_init_with_row_address_mode(Protomatter_core *core, uint16_t bitWidth, uint8_t bitDepth,
    uint8_t rgbCount, uint8_t *rgbList, uint8_t addrCount, uint8_t *addrList, uint8_t clockPin,
    uint8_t latchPin, uint8_t oePin, bool doubleBuffer, int8_t tile, void *timer,
    ProtomatterRowAddressMode rowAddressMode) {
    (void)addrList;
    (void)latchPin;
    (void)oePin;
    if (bitWidth == 0 || bitDepth < 1 || bitDepth > 6 || rgbCount < 1 || rgbCount > 5 || addrCount > 5 || tile == 0) {
        return PROTOMATTER_ERR_ARG;
    }
    core->timer = timer;
    core->rgbPins = rgbList;        // set: the binding's sign of a live matrix
    core->clockPin = clockPin;
    core->width = bitWidth;
    core->numPlanes = bitDepth;
    core->parallel = rgbCount;
    core->numAddressLines = addrCount;
    core->numRowPairs = 1 << addrCount;
    core->rowAddressMode = rowAddressMode;
    core->tile = tile;
    core->doubleBuffer = doubleBuffer;
    return PROTOMATTER_OK;
}

ProtomatterStatus _PM_begin(Protomatter_core *core) {
    int height = _height(core);
    if (height > 0xffff) {
        return PROTOMATTER_ERR_ARG;
    }
    _display = vdisplay_by_name("MATRIX");
    if (_display == NULL) {
        _display = vdisplay_add("MATRIX", core->width, (uint16_t)height);
    } else if (!vdisplay_resize(_display, core->width, (uint16_t)height)) {
        _display = NULL;
    }
    if (_display == NULL) {
        return PROTOMATTER_ERR_MALLOC;
    }
    _display->on = true;
    vdisplay_changed(_display);
    if (!_ticking) {
        supervisor_enable_tick();
        _ticking = true;
    }
    return PROTOMATTER_OK;
}

void _PM_convert_565(Protomatter_core *core, uint16_t *source, uint16_t width) {
    if (_display == NULL || core != _PM_protoPtr || width != _display->width) {
        return;
    }
    memcpy(_display->pixels, source, (size_t)_display->width * _display->height * sizeof(uint16_t));
    vdisplay_changed(_display);
}

void _PM_swapbuffer_maybe(Protomatter_core *core) {
    (void)core;
}

void _PM_stop(Protomatter_core *core) {
    if (_display != NULL && core == _PM_protoPtr) {
        _display->on = false;
        vdisplay_changed(_display);
    }
}

void _PM_resume(Protomatter_core *core) {
    if (_display != NULL && core == _PM_protoPtr) {
        _display->on = true;
        vdisplay_changed(_display);
    }
}

void _PM_deallocate(Protomatter_core *core) {
    // Deinit: the matrix goes dark.
    if (_display != NULL) {
        _display->on = false;
        vdisplay_changed(_display);
    }
    core->rgbPins = NULL;
    if (_ticking) {
        supervisor_disable_tick();
        _ticking = false;
    }
}

#endif
