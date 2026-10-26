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

int gamepad_key_button(SDL_Keycode key) {
    for (int i = 0; key != SDLK_UNKNOWN && i < DESKTOP_BUTTON_COUNT; i++) {
        for (int k = 0; k < KEYS_MAX; k++) {
            if (_keys[i][k] == key) {
                return i;
            }
        }
    }
    return -1;
}

const char *gamepad_key_name(int button) {
    if (button < 0 || button >= DESKTOP_BUTTON_COUNT || _keys[button][0] == SDLK_UNKNOWN) {
        return "";
    }
    return SDL_GetKeyName(_keys[button][0]);
}

bool gamepad_set_keys(const char *name, const char *keys, const char **why) {
    static char message[128];
    int button = gamepad_button(name);
    if (button < 0) {
        snprintf(message, sizeof(message), "no gamepad button %s (UP, DOWN, LEFT, RIGHT, A, B, X, Y, ...)", name);
        *why = message;
        return false;
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
        int had = gamepad_key_button(found[k]);
        for (int j = 0; had >= 0 && j < KEYS_MAX; j++) {
            if (_keys[had][j] == found[k]) {
                _keys[had][j] = SDLK_UNKNOWN;
            }
        }
    }
    memcpy(_keys[button], found, sizeof(found));
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
    if (button < 0 || button >= DESKTOP_BUTTON_COUNT) {
        return;
    }
    desktop_button_set(button, down);
    bus_trace_text("GAMEPAD %s %s", _names[button], down ? "pressed" : "released");
    panel_changed();
}

bool gamepad_pressed(int button) {
    return desktop_button_held(button);
}
