// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>

#include <SDL.h>

// The handheld's buttons (the virtual board's SW_ pins), pressed by keys, a game controller, the
// key script or the mouse on the board panel's gamepad. Keys and the panel run on the VM thread.

// UP, DOWN, ... MENU: the button's index (DESKTOP_BUTTON_*), or -1.
int gamepad_button(const char *name);
const char *gamepad_name(int button);
// The button a key presses, or -1.
int gamepad_key_button(SDL_Keycode key);
// The first key that presses a button, as SDL names it; "" for none.
const char *gamepad_key_name(int button);
// A [gamepad] line: the button's keys, comma separated, in place of its defaults; a key also
// leaves the button it had. False with *why set when a name is wrong.
bool gamepad_set_keys(const char *name, const char *keys, const char **why);
// The board file has a [gamepad]: the panel shows it.
void gamepad_show(void);
bool gamepad_shown(void);
// A button pressed or released: its pin, the trace, the panel.
void gamepad_press(int button, bool down);
// Held down now by a key, a controller, the script or the mouse.
bool gamepad_pressed(int button);
