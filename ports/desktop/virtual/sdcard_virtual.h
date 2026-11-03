// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// An SD card on the virtual SPI bus, answering as a card does in SPI mode, so sdcardio.SDCard and
// adafruit_sdcard work. Its blocks are an image file beside the drive (CIRCUITPY-SD.img, 16 MB),
// made and FAT-formatted at the card's first use, as a new card comes formatted.

// A card listening on cs; a second on the same cs is the same card. False with *why set.
bool sdcard_virtual_add(int cs, const char **why);
// The card selected now (its cs low), or -1.
int sdcard_virtual_selected(void);
// A transfer to the selected card: out (or fill) from the host, in (when not NULL) its answers.
void sdcard_virtual_transfer(int card, const uint8_t *out, uint8_t fill, uint8_t *in, size_t len);
// A command start (0x40 | CMD0) seen with no card selected: one is put on the low pin that is not
// a display's (automatic, announced), when there is such a pin. True when it was.
bool sdcard_virtual_attach_default(const uint8_t *out, size_t len);
