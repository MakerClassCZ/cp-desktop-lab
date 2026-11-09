// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shared-bindings/epaperdisplay/EPaperDisplay.h"

// An e-paper panel on the virtual SPI bus, made when a program constructs an EPaperDisplay on a
// fourwire bus: its RAM is written as the controller's (windows, current position, the black and
// the colour RAM), and only a refresh shows it, after which the busy pin is held for the refresh
// time. A board file's [spi] line "epaper cs=PIN [refresh_ms=N] [stuck]" names it and sets how
// long a refresh is busy, or keeps it busy for ever.

// Called from EPaperDisplay's construction (shared-module/epaperdisplay), before its start sequence.
void epaperdisplay_port_attach(epaperdisplay_epaperdisplay_obj_t *self, const epaperdisplay_construct_args_t *args);
// A board file's line; false with *why set when it is wrong.
bool epaper_virtual_declare(const char *name, const char *spec, int cs, const char **why);
// The bytes of a transfer while a panel's chip select is low; false when none is selected.
bool epaper_virtual_transfer(const uint8_t *out, uint8_t fill, size_t len);
// From the background task: a refresh's busy time ends.
void epaper_virtual_task(void);
bool epaper_virtual_uses_pin(int pin);
