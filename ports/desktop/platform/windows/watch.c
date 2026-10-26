// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// A directory CIRCUITPY reloads code.py when the host changes a file in it, as a board does when
// a computer writes to its drive. Hidden files and directories (editor swap files, .git) do not
// count, nor does boot_out.txt, nor anything while Python itself may write the drive.
// This system's watcher: ReadDirectoryChangesW on the whole tree.

#include "py/mpconfig.h"

#if CIRCUITPY_FILESYSTEM_POSIX

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "desktop_port.h"

static HANDLE _dir = INVALID_HANDLE_VALUE;

// Any component of the relative path starting with '.'.
static bool _hidden(const wchar_t *name, size_t len) {
    bool start = true;
    for (size_t i = 0; i < len; i++) {
        if (start && name[i] == L'.') {
            return true;
        }
        start = name[i] == L'\\' || name[i] == L'/';
    }
    return false;
}

static void *_watch(void *arg) {
    (void)arg;
    static uint8_t buf[64 * 1024] __attribute__((aligned(4)));
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    for (;;) {
        DWORD bytes = 0;
        if (!ReadDirectoryChangesW(_dir, buf, sizeof(buf), TRUE, filter, &bytes, NULL, NULL)) {
            return NULL;
        }
        bool changed = bytes == 0;      // too many changes for the buffer: something changed
        for (FILE_NOTIFY_INFORMATION *event = (FILE_NOTIFY_INFORMATION *)buf; bytes > 0;) {
            size_t len = event->FileNameLength / sizeof(wchar_t);
            if (!_hidden(event->FileName, len) &&
                !(len == 12 && _wcsnicmp(event->FileName, L"boot_out.txt", 12) == 0)) {
                changed = true;
            }
            if (event->NextEntryOffset == 0) {
                break;
            }
            event = (FILE_NOTIFY_INFORMATION *)((uint8_t *)event + event->NextEntryOffset);
        }
        if (changed) {
            desktop_watch_changed();
        }
    }
}

void desktop_watch_start(const char *root) {
    if (_dir != INVALID_HANDLE_VALUE) {
        return;
    }
    _dir = CreateFileA(root, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (_dir == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "circuitpython: cannot watch %s\r\n", root);
        return;
    }
    desktop_watch_thread(_watch, NULL);
}

#endif
