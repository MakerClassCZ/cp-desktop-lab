// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The panel's lines (set, press, turn, down, dump ...), one set for the control API and the key
// script, applied on the VM thread.

#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gamepad.h"
#include "midi_in.h"
#include "parts.h"
#include "vdisplay.h"

static const char *const _verbs[] = {
    "set", "press", "release", "tap", "turn", "send", "down", "up", "dump", "close",
};

// "dump NAME PATH": what a display shows, as a PPM.
static bool _dump(const char *line, char *err, size_t errlen) {
    char name[32];
    int used = 0;
    if (sscanf(line, "dump %31s %n", name, &used) != 1 || used == 0) {
        snprintf(err, errlen, "dump needs a display and a path");
        return false;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s", line + used);
    path[strcspn(path, "\r\n")] = '\0';
    vdisplay_t *d = vdisplay_by_name(name);
    if (d == NULL || !vdisplay_dump(d, path)) {
        snprintf(err, errlen, "cannot dump %s to %s", name, path);
        return false;
    }
    fprintf(stderr, "circuitpython: dumped %s to %s\r\n", name, path);
    return true;
}

bool panel_accepts(const char *line) {
    char verb[16];
    if (sscanf(line, "%15s", verb) != 1) {
        return false;
    }
    for (size_t i = 0; i < sizeof(_verbs) / sizeof(_verbs[0]); i++) {
        if (strcmp(verb, _verbs[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool panel_apply(const char *line, char *err, size_t errlen) {
    char verb[16], name[32];
    long value = 0;
    int n = sscanf(line, "%15s %31s %ld", verb, name, &value);
    if (!panel_accepts(line)) {
        snprintf(err, errlen, "not a panel command: %s", line);
        return false;
    }
    if (strcmp(verb, "dump") == 0) {
        return _dump(line, err, errlen);
    }
    if (n == 1 && strcmp(verb, "close") == 0) {
        panel_close();
        return true;
    }
    if (n >= 2 && (strcmp(verb, "down") == 0 || strcmp(verb, "up") == 0)) {
        // A key of the computer's keyboard, by its SDL name, as the key script plays it.
        char key[32];
        SDL_Event event = {0};
        event.type = verb[0] == 'd' ? SDL_KEYDOWN : SDL_KEYUP;
        event.key.state = event.type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
        event.key.keysym.sym = sscanf(line, "%*s %31[^\r\n]", key) == 1 ? SDL_GetKeyFromName(key) : SDLK_UNKNOWN;
        if (event.key.keysym.sym == SDLK_UNKNOWN) {
            snprintf(err, errlen, "no key %s", line + strlen(verb) + 1);
            return false;
        }
        // The key's position by its US name, so "down z" plays the same note on every layout.
        event.key.keysym.scancode = SDL_GetScancodeFromName(key);
        if (event.key.keysym.scancode == SDL_SCANCODE_UNKNOWN) {
            event.key.keysym.scancode = SDL_GetScancodeFromKey(event.key.keysym.sym);
        }
        SDL_PushEvent(&event);
        return true;
    }
    if (n >= 2 && midi_keyboard_present() && strcasecmp(name, midi_keyboard_name()) == 0
        && (strcmp(verb, "press") == 0 || strcmp(verb, "release") == 0 || strcmp(verb, "tap") == 0)) {
        // "press KEYS C4", "release KEYS 60", "tap KEYS C4" (pressed and let go).
        char note_text[16] = "";
        sscanf(line, "%*s %*s %15s", note_text);
        int note = midi_note_parse(note_text);
        if (note < 0) {
            snprintf(err, errlen, "bad note %s", note_text);
            return false;
        }
        midi_keyboard_note(note, verb[0] != 'r', 100);
        if (verb[0] == 't') {
            midi_keyboard_note(note, false, 0);
        }
        return true;
    }
    part_t *p = n >= 2 ? parts_by_name(name) : NULL;
    int button = n >= 2 && p == NULL ? gamepad_button(name) : -1;
    if (button >= 0 && (strcmp(verb, "press") == 0 || strcmp(verb, "release") == 0)) {
        gamepad_press(button, verb[0] == 'p');
    } else if (n < 2 || (strcmp(verb, "set") != 0 && strcmp(verb, "turn") != 0 && strcmp(verb, "send") != 0
                         && strcmp(verb, "press") != 0 && strcmp(verb, "release") != 0)) {
        snprintf(err, errlen, "not a panel command: %s", line);
        return false;
    } else if (p == NULL) {
        snprintf(err, errlen, "no part %s", name);
        return false;
    } else if (strcmp(verb, "set") == 0 && p->kind == PART_SENSOR) {
        char number[32];
        if (sscanf(line, "%*s %*s %31s", number) != 1) {
            snprintf(err, errlen, "set %s needs a value", name);
            return false;
        }
        parts_set_sensor(p, strtod(number, NULL));
    } else if (strcmp(verb, "set") == 0 && n == 3) {
        parts_set(p, (uint32_t)value);
    } else if (strcmp(verb, "turn") == 0 && n == 3) {
        parts_turn(p, (int)value);
    } else if (strcmp(verb, "send") == 0) {
        // "send NAME" its own code, "send NAME 255,2,247,8" another.
        char text[32] = "";
        uint8_t code[4];
        sscanf(line, "%*s %*s %31s", text);
        if (text[0] == '\0') {
            parts_ir_send(p, NULL);
        } else if (parts_ir_code(text, code)) {
            parts_ir_send(p, code);
        } else {
            snprintf(err, errlen, "bad IR code %s", text);
            return false;
        }
    } else if (strcmp(verb, "press") == 0 || strcmp(verb, "release") == 0) {
        parts_press(p, verb[0] == 'p');
    } else {
        snprintf(err, errlen, "%s %s needs a number", verb, name);
        return false;
    }
    return true;
}
