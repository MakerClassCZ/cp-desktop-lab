// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#ifdef _WIN32
#define MICROPY_HW_BOARD_NAME "Windows desktop"
#elif defined(__APPLE__)
#define MICROPY_HW_BOARD_NAME "macOS desktop"
#else
#define MICROPY_HW_BOARD_NAME "Linux desktop"
#endif
