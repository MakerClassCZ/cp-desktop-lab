// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "supervisor/board.h"
#include "common-hal/sdl/Framebuffer.h"

#include "desktop_pins.h"
#include "desktop_port.h"

void board_init(void) {
    if (desktop_pin_backend->accepts_drive) {
        desktop_buttons_release_all();
    }
    // A 320x240 picture, or the board file's, in a window twice its size.
    sdl_framebuffer_autoconstruct(desktop_display_width, desktop_display_height, 2, false);
}
