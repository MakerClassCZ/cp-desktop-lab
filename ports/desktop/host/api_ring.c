// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "api_ring.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

bool api_ring_init(api_ring_t *ring, size_t cap) {
    ring->data = malloc(cap);
    if (ring->data == NULL) {
        return false;
    }
    ring->cap = cap;
    ring->end = 0;
    pthread_mutex_init(&ring->lock, NULL);
    pthread_cond_init(&ring->grew, NULL);
    return true;
}

void api_ring_write(api_ring_t *ring, const char *data, size_t len) {
    pthread_mutex_lock(&ring->lock);
    if (len > ring->cap) {
        data += len - ring->cap;
        ring->end += len - ring->cap;
        len = ring->cap;
    }
    size_t at = (size_t)(ring->end % ring->cap);
    size_t first = len < ring->cap - at ? len : ring->cap - at;
    memcpy(ring->data + at, data, first);
    memcpy(ring->data, data + first, len - first);
    ring->end += len;
    pthread_cond_broadcast(&ring->grew);
    pthread_mutex_unlock(&ring->lock);
}

// The bytes [from, to) into out; the caller holds the lock and checked they are still held.
static void _copy(const api_ring_t *ring, uint64_t from, uint64_t to, char *out) {
    size_t len = (size_t)(to - from), at = (size_t)(from % ring->cap);
    size_t first = len < ring->cap - at ? len : ring->cap - at;
    memcpy(out, ring->data + at, first);
    memcpy(out + first, ring->data, len - first);
}

static char _at(const api_ring_t *ring, uint64_t offset) {
    return ring->data[offset % ring->cap];
}

// Where text first starts in [from, to), searched in the ring itself; to when it does not.
static uint64_t _find(const api_ring_t *ring, uint64_t from, uint64_t to, const char *text, size_t m) {
    for (uint64_t i = from; i + m <= to; i++) {
        size_t k = 0;
        while (k < m && _at(ring, i + k) == text[k]) {
            k++;
        }
        if (k == m) {
            return i;
        }
    }
    return to;
}

uint64_t api_ring_end(api_ring_t *ring) {
    pthread_mutex_lock(&ring->lock);
    uint64_t end = ring->end;
    pthread_mutex_unlock(&ring->lock);
    return end;
}

size_t api_ring_read(api_ring_t *ring, uint64_t since, const char *text, int timeout_ms,
    size_t max, char **out, api_ring_read_t *info) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    size_t m = text != NULL ? strlen(text) : 0;
    memset(info, 0, sizeof(*info));
    // Each wake searches only what came since the last look, and the m - 1 bytes before it, so a
    // writer (the VM) never waits long for the lock.
    uint64_t searched = 0;
    pthread_mutex_lock(&ring->lock);
    for (;;) {
        uint64_t oldest = ring->end > ring->cap ? ring->end - ring->cap : 0;
        uint64_t from = since < oldest ? oldest : since > ring->end ? ring->end : since;
        info->lost = since < oldest;
        info->start = from;
        info->end = ring->end;
        info->next = ring->end;
        if (m > 0) {
            uint64_t look = searched + 1 > from + m ? searched + 1 - m : from;
            uint64_t hit = _find(ring, look, ring->end, text, m);
            if (hit != ring->end) {
                info->matched = true;
                info->next = hit + m;
                break;
            }
            searched = ring->end;
        } else if (ring->end > from) {
            break;
        }
        if (timeout_ms <= 0 || pthread_cond_timedwait(&ring->grew, &ring->lock, &deadline) != 0) {
            // One last look, then the time is up.
            if (timeout_ms <= 0) {
                break;
            }
            timeout_ms = 0;
        }
    }
    size_t n = (size_t)(info->end - info->start);
    if (max == 0) {
        n = 0;                  // only whether the text came
    } else if (n > max) {
        n = max;
        info->end = info->start + n;
        // Never past what is returned: a match beyond it is found again on the next read.
        if (info->next > info->end) {
            info->next = info->end;
            info->matched = false;
        }
    }
    char *buf = malloc(n + 1);
    if (buf != NULL) {
        _copy(ring, info->start, info->start + n, buf);
        buf[n] = '\0';
    } else {
        n = 0;
    }
    pthread_mutex_unlock(&ring->lock);
    *out = buf;
    return n;
}
