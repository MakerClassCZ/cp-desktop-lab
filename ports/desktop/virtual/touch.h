// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "i2c_virtual.h"

// Touchscreens: a touch on a display (the mouse on it, or the panel line "touch NAME X Y") is
// read by what is bound to that display: a resistive panel on four pins ([touchscreen] in a board
// file) or an I2C touch controller (a model with .touch). DISPLAY is the main window
// (board.DISPLAY); other names are the board file's displays. On the VM thread.

#define TOUCH_MAIN_DISPLAY "DISPLAY"

// NAME = "resistive X1 X2 Y1 Y2 [display=D] [calibration=a,b,c,d] [swap] [flip_x] [flip_y]",
// the pins already looked up; false with *why set.
bool touch_add_resistive(const char *name, const uint8_t pins[4], const char *options, const char **why);
// An I2C touch controller attached: it follows display (NULL or "" = the default one), with the
// spec's swap/flip words.
void touch_bind_i2c(i2c_virtual_device_t *dev, const char *display, const char *options);

// A touch at x, y of what the display shows (down) or its end; false when nothing reads touches
// on that display or it has no such name.
bool touch_set(const char *display, bool down, int x, int y);
// Something reads touches on the display (the main window shows its cursor then).
bool touch_display_bound(const char *display);
// The display's touch, for the panel's state; false when nothing is bound to it.
bool touch_state(const char *display, bool *down, int *x, int *y);

// A pin of a resistive panel: AnalogIn reads its voltage from the panel's circuit (0-65535).
bool touch_owns_pin(uint8_t pin);
uint16_t touch_pin_value(uint8_t pin);
