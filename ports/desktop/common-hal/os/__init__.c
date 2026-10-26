// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/mpconfig.h"
#include "shared-bindings/os/__init__.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>

bool common_hal_os_urandom(uint8_t *buffer, mp_uint_t length) {
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, buffer, length, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

#else
#include <sys/random.h>

bool common_hal_os_urandom(uint8_t *buffer, mp_uint_t length) {
    while (length > 0) {
        ssize_t n = getrandom(buffer, length, 0);
        if (n <= 0) {
            return false;
        }
        buffer += n;
        length -= n;
    }
    return true;
}
#endif
