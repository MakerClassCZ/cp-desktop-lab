// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// microcontroller.nvm: CIRCUITPY_INTERNAL_NVM_SIZE bytes kept in a file beside the drive
// (desktop_options.nvm_path), not on it, as a board keeps them in flash outside CIRCUITPY.
typedef struct {
    mp_obj_base_t base;
} nvm_bytearray_obj_t;
