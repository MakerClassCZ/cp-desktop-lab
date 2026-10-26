// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The console on a terminal: termios raw mode, read() on a thread, write().

#include <errno.h>
#include <pthread.h>
#include <termios.h>
#include <unistd.h>

#include "platform.h"

static bool _tty;
static struct termios _saved_termios;
static void (*_received)(const uint8_t *data, size_t len);

void platform_console_restore(void) {
    if (_tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &_saved_termios);
    }
}

static void *_reader(void *arg) {
    (void)arg;
    uint8_t buf[64];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return NULL;                // end of input: the console stays output only
        }
        _received(buf, n);
    }
}

bool platform_console_open(void (*received)(const uint8_t *data, size_t len)) {
    _received = received;
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &_saved_termios) == 0) {
        struct termios raw = _saved_termios;
        raw.c_iflag &= ~(BRKINT | ICRNL | INLCR | IGNCR | ISTRIP | IXON | INPCK);
        raw.c_oflag &= ~OPOST;              // CircuitPython sends \r\n itself
        raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        _tty = true;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t reader;
    pthread_create(&reader, &attr, _reader, NULL);
    return _tty;
}

void platform_console_write(const char *text, uint32_t len) {
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, text, len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return;
        }
        text += n;
        len -= n;
    }
}
