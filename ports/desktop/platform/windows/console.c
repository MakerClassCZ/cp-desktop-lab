// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The console on Windows: the console's raw and virtual-terminal modes, ReadFile on a thread,
// WriteFile.

#include <windows.h>

#include "platform.h"

static HANDLE _in;
static HANDLE _out;
static HANDLE _reader_thread;
static DWORD _in_mode;
static DWORD _out_mode;
static bool _tty;
static bool _console_out;
static void (*_received)(const uint8_t *data, size_t len);

void platform_console_restore(void) {
    if (_tty) {
        SetConsoleMode(_in, _in_mode);
    }
    if (_console_out) {
        SetConsoleMode(_out, _out_mode);
    }
    if (_reader_thread != NULL) {
        // A restarted process reads the console next; this one must not take its keys.
        CancelSynchronousIo(_reader_thread);
    }
}

static DWORD WINAPI _reader(LPVOID arg) {
    (void)arg;
    uint8_t buf[64];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(_in, buf, sizeof(buf), &n, NULL) || n == 0) {
            return 0;                   // end of input: the console stays output only
        }
        _received(buf, n);
    }
}

bool platform_console_open(void (*received)(const uint8_t *data, size_t len)) {
    _received = received;
    _in = GetStdHandle(STD_INPUT_HANDLE);
    _out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(_in, &_in_mode)) {
        // Raw: no line editing or echo, Ctrl-C as a byte, keys as escape sequences.
        DWORD mode = _in_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        SetConsoleMode(_in, mode | ENABLE_VIRTUAL_TERMINAL_INPUT);
        _tty = true;
    }
    if (GetConsoleMode(_out, &_out_mode)) {
        SetConsoleMode(_out, _out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
        _console_out = true;
    }
    _reader_thread = CreateThread(NULL, 0, _reader, NULL, 0, NULL);
    return _tty;
}

void platform_console_write(const char *text, uint32_t len) {
    while (len > 0) {
        DWORD n = 0;
        if (!WriteFile(_out, text, len, &n, NULL)) {
            return;
        }
        text += n;
        len -= n;
    }
}
