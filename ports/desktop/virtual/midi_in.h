// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// MIDI into the program: the virtual keyboard and the CC sliders (and later a device of the
// computer) queue messages, usb_midi.ports[0] reads them. The queue belongs to one run of the
// program: each start, reload and reset begins with it empty.

#define MIDI_IN_QUEUE 1024

// A whole message queued and traced; the oldest are dropped when the queue is full.
void midi_in_put(const uint8_t *msg, size_t len);
// Up to max bytes, 0 when nothing came. A run's first read queues the CC sliders' values first.
size_t midi_in_read(uint8_t *buf, size_t max);
// A new run: the queue emptied, the keys held let go on the panel only.
void midi_in_reset(void);

// "C4" (60), "C#3", "C-1" (0) or a number 0-127; -1 when it is none.
int midi_note_parse(const char *text);
// 60 -> "C4".
void midi_note_name(int note, char *buf, size_t len);

// The keyboard: options "C2", "channel=2", both, or none. False with *why set when they are wrong
// or another keyboard is on the board.
bool midi_keyboard_add(const char *name, const char *options, const char **why);
// Pulled out: its held notes get their Note Offs.
void midi_keyboard_remove(void);
bool midi_keyboard_present(void);
const char *midi_keyboard_name(void);
int midi_keyboard_low(void);           // the lowest note it shows
int midi_keyboard_channel(void);       // 1-16
bool midi_keyboard_held(int note);
// A key pressed or let go: Note On (velocity 1-127) or Note Off queued, a repeat ignored.
void midi_keyboard_note(int note, bool down, int velocity);
// A computer key (an SDL scancode): true when it is the keyboard's, played or moving the octave.
bool midi_keyboard_key(int scancode, bool down);
// The computer key that plays a key of the keyboard (0-24 from its lowest note), or 0 for none.
int midi_keyboard_scancode(int offset);
// The keyboard an octave up (1) or down (-1), its held notes let go first.
void midi_keyboard_shift(int octaves);
// Every held note let go.
void midi_keyboard_release_all(void);
