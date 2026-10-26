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
    // A 320x240 picture in a window twice its size.
    sdl_framebuffer_autoconstruct(320, 240, 2, false);
}
