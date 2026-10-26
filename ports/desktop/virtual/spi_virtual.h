// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "desktop_pins.h"
#include "vdisplay.h"

// The virtual board's SPI: a virtual bus on any pins with the board file's [spi] devices on it,
// NAME = "MODEL WIDTHxHEIGHT cs=PIN dc=PIN". A device listens while its chip select pin is low;
// its data/command pin tells commands from data, as fourwire drives them.

extern const desktop_spi_ops_t spi_virtual_ops;

// A board file line, the pins already looked up: false with *why set when it is wrong.
bool spi_virtual_add(const char *name, const char *spec, int cs, int dc, const char **why);
// The display selected by the cs pin, or NULL.
vdisplay_t *spi_virtual_display(int cs);
// The rotation the program gives its display (0, 90, 180, 270): the panel shows it undone.
void spi_virtual_set_rotation(int cs, uint16_t rotation);
