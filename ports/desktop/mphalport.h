// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <errno.h>

#include "py/obj.h"
#include "lib/oofatfs/ff.h"
#include "supervisor/shared/tick.h"

static inline mp_uint_t mp_hal_ticks_ms(void) {
    return supervisor_ticks_ms32();
}

void mp_hal_set_interrupt_char(int c);

// VfsPosix retries a system call that a signal cut short.
#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) \
    { \
        for (;;) { \
            ret = syscall; \
            if (ret == -1) { \
                int err = errno; \
                if (err == EINTR) { \
                    mp_handle_pending(true); \
                    continue; \
                } \
                raise; \
            } \
            break; \
        } \
    }

void mp_hal_disable_all_interrupts(void);
void mp_hal_enable_all_interrupts(void);
