// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>

#include <SDL.h>

// A handheld's buttons, pressed by keys, a game controller, the key script or the mouse on the
// board panel's gamepad. A board file brings them: [gamepad] puts a button on a pin, [shiftreg]
// behind a shift register; a button it does not name is not there. Keys and the panel run on the
// VM thread.

// UP, DOWN, ... MENU: the button's index (DESKTOP_BUTTON_*), or -1.
int gamepad_button(const char *name);
const char *gamepad_name(int button);
// The board file has the button.
bool gamepad_used(int button);
// A shift register reads the button.
void gamepad_use(int button);
// A [gamepad] line, "pin=PIN keys=KEY, KEY": the button on the pin (pressed reads low) and its keys
// in place of its defaults; a key also leaves the button it had. pin is a GPIO number, -1 for none.
bool gamepad_add(int button, int pin, const char *keys, const char **why);
// The button a key presses, or -1.
int gamepad_key_button(SDL_Keycode key);
// The first key that presses a button, as SDL names it, of those the MIDI keyboard leaves it; ""
// for none.
const char *gamepad_key_name(int button);
// After the board files: false with *why set when a key a [gamepad] line gives plays the keyboard.
bool gamepad_check_keys(const char **why);
// The board file has a [gamepad]: the panel shows it.
void gamepad_show(void);
bool gamepad_shown(void);
// A button pressed or released: its pin, the trace, the panel. A button not there is left alone.
void gamepad_press(int button, bool down);
// Held down now by a key, a controller, the script or the mouse.
bool gamepad_pressed(int button);
// Every button let go, its pin reading high: at each start.
void gamepad_release_all(void);
