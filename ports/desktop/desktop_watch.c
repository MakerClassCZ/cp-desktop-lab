// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// A directory CIRCUITPY reloads code.py when the host changes a file in it, as a board does when
// a computer writes to its drive. Hidden files and directories (editor swap files, .git) do not
// count, nor does boot_out.txt, nor anything while Python itself may write the drive.

#include "py/mpconfig.h"

#if CIRCUITPY_FILESYSTEM_POSIX

#include <pthread.h>

#include "supervisor/filesystem.h"
#include "supervisor/shared/reload.h"

#include "desktop_port.h"

void desktop_watch_changed(void) {
    supervisor_vfs_t *circuitpy = filesystem_circuitpy();
    if (circuitpy != NULL && !filesystem_is_writable_by_python(circuitpy)) {
        autoreload_trigger();
        desktop_wake_vm();
    }
}

void desktop_watch_thread(void *(*main)(void *), void *arg) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, main, arg) == 0) {
        pthread_detach(thread);
    }
}

#endif
