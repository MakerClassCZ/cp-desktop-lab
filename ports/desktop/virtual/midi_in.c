// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "midi_in.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL.h>

#include "bus_trace.h"
#include "panel.h"

#define KEYBOARD_LOW 48     // C3
#define KEYBOARD_KEYS 25    // two octaves, C to C

// The computer keys by position, as trackers lay them out: the lower octave on Z X C ... with its
// black keys on the row above, the upper on Q W E ... with its black keys on the digits. Each is
// the semitone above the keyboard's lowest note.
static const struct {
    SDL_Scancode key;
    int8_t offset;
} _keys[] = {
    { SDL_SCANCODE_Z, 0 }, { SDL_SCANCODE_S, 1 }, { SDL_SCANCODE_X, 2 }, { SDL_SCANCODE_D, 3 },
    { SDL_SCANCODE_C, 4 }, { SDL_SCANCODE_V, 5 }, { SDL_SCANCODE_G, 6 }, { SDL_SCANCODE_B, 7 },
    { SDL_SCANCODE_H, 8 }, { SDL_SCANCODE_N, 9 }, { SDL_SCANCODE_J, 10 }, { SDL_SCANCODE_M, 11 },
    { SDL_SCANCODE_COMMA, 12 }, { SDL_SCANCODE_L, 13 }, { SDL_SCANCODE_PERIOD, 14 },
    { SDL_SCANCODE_SEMICOLON, 15 }, { SDL_SCANCODE_SLASH, 16 },
    { SDL_SCANCODE_Q, 12 }, { SDL_SCANCODE_2, 13 }, { SDL_SCANCODE_W, 14 }, { SDL_SCANCODE_3, 15 },
    { SDL_SCANCODE_E, 16 }, { SDL_SCANCODE_R, 17 }, { SDL_SCANCODE_5, 18 }, { SDL_SCANCODE_T, 19 },
    { SDL_SCANCODE_6, 20 }, { SDL_SCANCODE_Y, 21 }, { SDL_SCANCODE_7, 22 }, { SDL_SCANCODE_U, 23 },
    { SDL_SCANCODE_I, 24 }, { SDL_SCANCODE_9, 25 }, { SDL_SCANCODE_O, 26 }, { SDL_SCANCODE_0, 27 },
    { SDL_SCANCODE_P, 28 },
};

static pthread_mutex_t _lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t _queue[MIDI_IN_QUEUE];
static size_t _head, _used;         // the oldest byte, how many there are
static size_t _lost;                // bytes dropped this run
static bool _read;                  // the program has read this run
static bool _lost_traced;

static bool _present;
static char _name[24];
static int _low = KEYBOARD_LOW;
static int _channel = 1;
static bool _held[128];

// The oldest message dropped: its status byte and the data bytes after it.
static void _drop(void) {
    do {
        _head = (_head + 1) % MIDI_IN_QUEUE;
        _used--;
        _lost++;
    } while (_used > 0 && !(_queue[_head] & 0x80));
}

void midi_in_put(const uint8_t *msg, size_t len) {
    if (len == 0 || len > MIDI_IN_QUEUE) {
        return;
    }
    char hex[3 * 16 + 1] = "";
    for (size_t k = 0; k < len && k < 16; k++) {
        snprintf(hex + 3 * k, 4, " %02x", msg[k]);
    }
    bus_trace_text("MIDI IN %s", hex + 1);
    pthread_mutex_lock(&_lock);
    while (_used + len > MIDI_IN_QUEUE) {
        _drop();
    }
    for (size_t k = 0; k < len; k++) {
        _queue[(_head + _used + k) % MIDI_IN_QUEUE] = msg[k];
    }
    _used += len;
    // Only a run that reads MIDI cares what it lost.
    bool trace = _lost > 0 && _read && !_lost_traced;
    _lost_traced = _lost_traced || trace;
    size_t lost = _lost;
    pthread_mutex_unlock(&_lock);
    if (trace) {
        bus_trace_text("MIDI IN lost %u bytes", (unsigned)lost);
    }
}

size_t midi_in_read(uint8_t *buf, size_t max) {
    pthread_mutex_lock(&_lock);
    _read = true;
    size_t n = _used < max ? _used : max;
    for (size_t k = 0; k < n; k++) {
        buf[k] = _queue[(_head + k) % MIDI_IN_QUEUE];
    }
    _head = (_head + n) % MIDI_IN_QUEUE;
    _used -= n;
    pthread_mutex_unlock(&_lock);
    return n;
}

void midi_in_reset(void) {
    pthread_mutex_lock(&_lock);
    _head = _used = _lost = 0;
    _read = _lost_traced = false;
    pthread_mutex_unlock(&_lock);
    memset(_held, 0, sizeof(_held));
    panel_changed();
}

int midi_note_parse(const char *text) {
    static const int steps[] = { 9, 11, 0, 2, 4, 5, 7 };    // A B C D E F G
    char *end;
    long n = strtol(text, &end, 10);
    if (end != text && *end == '\0') {
        return n >= 0 && n <= 127 ? (int)n : -1;
    }
    char letter = (char)toupper((unsigned char)text[0]);
    if (letter < 'A' || letter > 'G') {
        return -1;
    }
    int note = steps[letter - 'A'];
    const char *s = text + 1;
    if (*s == '#') {
        note++;
        s++;
    }
    long octave = strtol(s, &end, 10);
    if (end == s || *end != '\0' || octave < -1 || octave > 9) {
        return -1;
    }
    note += (int)(octave + 1) * 12;
    return note <= 127 ? note : -1;
}

void midi_note_name(int note, char *buf, size_t len) {
    static const char *const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    snprintf(buf, len, "%s%d", names[note % 12], note / 12 - 1);
}

bool midi_keyboard_add(const char *name, const char *options, const char **why) {
    if (_present && strcasecmp(_name, name) != 0) {
        *why = "one keyboard on a board";
        return false;
    }
    int low = KEYBOARD_LOW, channel = 1;
    char copy[64];
    snprintf(copy, sizeof(copy), "%s", options);
    for (char *word = strtok(copy, " "); word != NULL; word = strtok(NULL, " ")) {
        if (strncmp(word, "channel=", 8) == 0) {
            channel = atoi(word + 8);
            if (channel < 1 || channel > 16) {
                *why = "bad channel (1-16)";
                return false;
            }
        } else if ((low = midi_note_parse(word)) < 0 || low > 103) {
            *why = "bad keyboard (its lowest note, \"C3\", and \"channel=N\")";
            return false;
        }
    }
    bool added = !_present;
    midi_keyboard_remove();
    _present = true;
    snprintf(_name, sizeof(_name), "%s", name);
    _low = low;
    _channel = channel;
    if (added) {
        panel_part_added();
    }
    panel_changed();
    return true;
}

void midi_keyboard_remove(void) {
    midi_keyboard_release_all();
    _present = false;
    panel_changed();
}

bool midi_keyboard_present(void) {
    return _present;
}

const char *midi_keyboard_name(void) {
    return _name;
}

int midi_keyboard_low(void) {
    return _low;
}

int midi_keyboard_channel(void) {
    return _channel;
}

bool midi_keyboard_held(int note) {
    return note >= 0 && note < 128 && _held[note];
}

void midi_keyboard_note(int note, bool down, int velocity) {
    if (!_present || note < 0 || note > 127 || _held[note] == down) {
        return;
    }
    _held[note] = down;
    uint8_t msg[3] = { (uint8_t)((down ? 0x90 : 0x80) | (_channel - 1)), (uint8_t)note,
                       (uint8_t)(down ? (velocity < 1 ? 1 : velocity > 127 ? 127 : velocity) : 0) };
    midi_in_put(msg, sizeof(msg));
    panel_changed();
}

bool midi_keyboard_key(int scancode, bool down) {
    if (!_present) {
        return false;
    }
    if (scancode == SDL_SCANCODE_PAGEUP || scancode == SDL_SCANCODE_PAGEDOWN) {
        if (down) {
            midi_keyboard_shift(scancode == SDL_SCANCODE_PAGEUP ? 1 : -1);
        }
        return true;
    }
    for (size_t i = 0; i < sizeof(_keys) / sizeof(_keys[0]); i++) {
        if ((int)_keys[i].key == scancode) {
            // A key past the keyboard's top note plays nothing, but is still the keyboard's.
            if (_keys[i].offset < KEYBOARD_KEYS) {
                midi_keyboard_note(_low + _keys[i].offset, down, 100);
            }
            return true;
        }
    }
    return false;
}

int midi_keyboard_scancode(int offset) {
    // Where both rows play a key, the upper row's: the lower row ends where the upper begins.
    int key = 0;
    for (size_t i = 0; i < sizeof(_keys) / sizeof(_keys[0]); i++) {
        if (_keys[i].offset == offset) {
            key = _keys[i].key;
        }
    }
    return key;
}

void midi_keyboard_shift(int octaves) {
    int low = _low + 12 * octaves;
    if (low < 12 || low > 96) {         // C0 to C7: its top C is 127 at most
        return;
    }
    midi_keyboard_release_all();
    _low = low;
    bus_trace_text("MIDI %s octave %+d", _name, octaves);
    panel_changed();
}

void midi_keyboard_release_all(void) {
    for (int note = 0; note < 128; note++) {
        midi_keyboard_note(note, false, 0);
    }
}
