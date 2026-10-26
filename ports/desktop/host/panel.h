// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>

#include <SDL.h>

#include "api_json.h"

// The board panel: one window with the NeoPixels (neopixel_emu.c) and a row per virtual part
// (parts.c), its name, its control and its exact value. Opens once there is something to show.

void panel_changed(void);           // something to draw again
void panel_task(void);              // from the SDL task: open, draw
bool panel_open(void);
bool panel_event(const SDL_Event *event);   // true when the event was the panel's
// A key script line for the panel: "set NAME VALUE", "press NAME", "release NAME", "turn NAME
// DETENTS". From any thread.
bool panel_command(const char *line);
// The same line applied now, on the VM thread, with "down KEY" and "up KEY" too; false: why in err.
bool panel_apply(const char *line, char *err, size_t errlen);

// What the panel shows, as JSON: the parts, the NeoPixels, the displays, the gamepad's buttons.
// On the VM thread.
void panel_state_json(api_buf_t *b);
