// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A stream kept for the control API's readers (the console, the trace): the last bytes written,
// addressed by an offset that only grows, so a reader asks for what came since its last read.

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t grew;
    char *data;
    size_t cap;
    uint64_t end;               // bytes written so far; the ring holds the last cap of them
} api_ring_t;

bool api_ring_init(api_ring_t *ring, size_t cap);
// Never waits for a reader, only for the lock.
void api_ring_write(api_ring_t *ring, const char *data, size_t len);

// The offset after the last byte written.
uint64_t api_ring_end(api_ring_t *ring);

typedef struct {
    uint64_t start, end;        // the bytes returned: [start, end)
    uint64_t next;              // where the next read starts: past the match, else end
    bool lost;                  // what was asked for began before the ring's oldest byte
    bool matched;
} api_ring_read_t;

// What came from since on, into a malloc'd *out (NUL-terminated), at most max bytes from the
// oldest asked for. With text: waits until it appears after since, at most timeout_ms. Without:
// waits until anything is there, at most timeout_ms (0: not at all). Returns the bytes' count.
// max 0: no bytes, only start, end and whether text came.
size_t api_ring_read(api_ring_t *ring, uint64_t since, const char *text, int timeout_ms,
    size_t max, char **out, api_ring_read_t *info);
