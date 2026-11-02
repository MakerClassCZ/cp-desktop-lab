// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "gamepad.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "bus_trace.h"
#include "desktop_pins.h"
#include "desktop_port.h"
#include "midi_in.h"
#include "panel.h"

#define KEYS_MAX 4

static const char *const _names[DESKTOP_BUTTON_COUNT] = {
    "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "START", "SELECT", "L", "R", "MENU",
};

// The arrows and Z X A S sit where a handheld's buttons are.
static SDL_Keycode _keys[DESKTOP_BUTTON_COUNT][KEYS_MAX] = {
    { SDLK_UP }, { SDLK_DOWN }, { SDLK_LEFT }, { SDLK_RIGHT },
    { SDLK_z }, { SDLK_x }, { SDLK_a }, { SDLK_s },
    { SDLK_RETURN }, { SDLK_BACKSPACE }, { SDLK_q }, { SDLK_w }, { SDLK_ESCAPE },
};

static bool _shown;
static bool _used[DESKTOP_BUTTON_COUNT];
static bool _held[DESKTOP_BUTTON_COUNT];
static bool _given[DESKTOP_BUTTON_COUNT];      // keys= in the board file
// Each button's pin plus one; 0 for none.
static uint8_t _pin[DESKTOP_BUTTON_COUNT];

int gamepad_button(const char *name) {
    for (int i = 0; i < DESKTOP_BUTTON_COUNT; i++) {
        if (strcasecmp(name, _names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *gamepad_name(int button) {
    return button >= 0 && button < DESKTOP_BUTTON_COUNT ? _names[button] : "";
}

bool gamepad_used(int button) {
    return button >= 0 && button < DESKTOP_BUTTON_COUNT && _used[button];
}

void gamepad_use(int button) {
    if (button >= 0 && button < DESKTOP_BUTTON_COUNT) {
        _used[button] = true;
    }
}

int gamepad_key_button(SDL_Keycode key) {
    for (int i = 0; key != SDLK_UNKNOWN && i < DESKTOP_BUTTON_COUNT; i++) {
        if (!_used[i]) {
            continue;
        }
        for (int k = 0; k < KEYS_MAX; k++) {
            if (_keys[i][k] == key) {
                return i;
            }
        }
    }
    return -1;
}

const char *gamepad_key_name(int button) {
    for (int k = 0; button >= 0 && button < DESKTOP_BUTTON_COUNT && k < KEYS_MAX; k++) {
        if (_keys[button][k] != SDLK_UNKNOWN && !midi_keyboard_takes(_keys[button][k])) {
            return SDL_GetKeyName(_keys[button][k]);
        }
    }
    return "";
}

bool gamepad_check_keys(const char **why) {
    static char message[96];
    for (int i = 0; i < DESKTOP_BUTTON_COUNT; i++) {
        for (int k = 0; _given[i] && k < KEYS_MAX; k++) {
            if (midi_keyboard_takes(_keys[i][k])) {
                snprintf(message, sizeof(message), "[gamepad] %s: %s plays the [midi] keyboard", _names[i],
                    SDL_GetKeyName(_keys[i][k]));
                *why = message;
                return false;
            }
        }
    }
    return true;
}

bool gamepad_add(int button, int pin, const char *keys, const char **why) {
    static char message[128];
    _used[button] = true;
    _pin[button] = pin >= 0 ? (uint8_t)(pin + 1) : 0;
    if (keys == NULL) {
        return true;
    }
    SDL_Keycode found[KEYS_MAX] = { SDLK_UNKNOWN };
    int n = 0;
    char list[128];
    snprintf(list, sizeof(list), "%s", keys);
    for (char *key = strtok(list, ","); key != NULL; key = strtok(NULL, ",")) {
        while (isspace((unsigned char)*key)) {
            key++;
        }
        char *end = key + strlen(key);
        while (end > key && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }
        SDL_Keycode code = SDL_GetKeyFromName(key);
        if (code == SDLK_UNKNOWN) {
            snprintf(message, sizeof(message), "unknown key %s (SDL names: Space, W, Up, Return, ...)", key);
            *why = message;
            return false;
        }
        if (n == KEYS_MAX) {
            *why = "more than 4 keys on one button";
            return false;
        }
        found[n++] = code;
    }
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < DESKTOP_BUTTON_COUNT; i++) {
            for (int j = 0; j < KEYS_MAX; j++) {
                if (_keys[i][j] == found[k]) {
                    _keys[i][j] = SDLK_UNKNOWN;
                }
            }
        }
    }
    memcpy(_keys[button], found, sizeof(found));
    _given[button] = true;
    return true;
}

void gamepad_show(void) {
    _shown = true;
    panel_changed();
}

bool gamepad_shown(void) {
    return _shown;
}

void gamepad_press(int button, bool down) {
    if (!gamepad_used(button)) {
        return;
    }
    _held[button] = down;
    if (_pin[button] != 0) {
        desktop_pin_drive(_pin[button] - 1, !down);
    }
    bus_trace_text("GAMEPAD %s %s", _names[button], down ? "pressed" : "released");
    panel_changed();
}

bool gamepad_pressed(int button) {
    return gamepad_used(button) && _held[button];
}

void gamepad_release_all(void) {
    for (int i = 0; i < DESKTOP_BUTTON_COUNT; i++) {
        _held[i] = false;
        if (_pin[i] != 0) {
            desktop_pin_drive(_pin[i] - 1, true);
        }
    }
}
