// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <string.h>

#include "audio_capture.h"
#include "desktop_pins.h"

#define RECORDINGS 8
// Per recording: ten minutes of 48 kHz stereo; what comes after is counted, not kept.
#define MAX_SAMPLES (48000u * 2 * 600)

typedef struct {
    uint8_t pin;
    uint8_t channels;
    uint32_t rate;
    int16_t *samples;
    size_t count, cap;
    size_t dropped;         // frames past MAX_SAMPLES
    size_t underruns;
} recording_t;

static recording_t _rec[RECORDINGS];
static size_t _count;
static bool _on;

static void _clear(void) {
    for (size_t i = 0; i < _count; i++) {
        free(_rec[i].samples);
    }
    memset(_rec, 0, sizeof(_rec));
    _count = 0;
}

void audio_capture_start(void) {
    _clear();
    _on = true;
}

void audio_capture_stop(void) {
    _on = false;
}

bool audio_capture_on(void) {
    return _on;
}

// The pin's recording at this rate and channel count; a new one when either changed.
static recording_t *_recording(uint8_t pin, uint32_t rate, uint8_t channels) {
    for (size_t i = _count; i > 0; i--) {
        recording_t *r = &_rec[i - 1];
        if (r->pin == pin) {
            if (r->rate == rate && r->channels == channels) {
                return r;
            }
            break;
        }
    }
    if (_count == RECORDINGS) {
        return NULL;
    }
    recording_t *r = &_rec[_count++];
    r->pin = pin;
    r->rate = rate;
    r->channels = channels;
    return r;
}

void audio_capture_add(uint8_t pin, uint32_t rate, uint8_t channels, const int16_t *samples, size_t count) {
    if (!_on || count == 0) {
        return;
    }
    recording_t *r = _recording(pin, rate, channels);
    if (r == NULL) {
        return;
    }
    size_t room = r->count < MAX_SAMPLES ? MAX_SAMPLES - r->count : 0;
    size_t keep = count < room ? count : room;
    if (r->count + keep > r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 65536;
        while (cap < r->count + keep) {
            cap *= 2;
        }
        int16_t *grown = realloc(r->samples, cap * sizeof(int16_t));
        if (grown == NULL) {
            keep = 0;
        } else {
            r->samples = grown;
            r->cap = cap;
        }
    }
    if (keep > 0) {
        memcpy(r->samples + r->count, samples, keep * sizeof(int16_t));
        r->count += keep;
    }
    r->dropped += (count - keep) / channels;
}

void audio_capture_underrun(uint8_t pin) {
    for (size_t i = _count; _on && i > 0; i--) {
        if (_rec[i - 1].pin == pin) {
            _rec[i - 1].underruns++;
            return;
        }
    }
}

void audio_capture_json(api_buf_t *b) {
    api_buf_printf(b, "{\"on\": %s, \"recordings\": [", _on ? "true" : "false");
    for (size_t i = 0; i < _count; i++) {
        const recording_t *r = &_rec[i];
        char name[32];
        desktop_pin_name(r->pin, name, sizeof(name));
        api_buf_printf(b, "%s{\"index\": %u, \"pin\": ", i ? ", " : "", (unsigned)i);
        api_buf_json_string(b, name, strlen(name));
        api_buf_printf(b, ", \"rate\": %lu, \"channels\": %u, \"frames\": %lu, \"dropped\": %lu, \"underruns\": %lu}",
            (unsigned long)r->rate, r->channels, (unsigned long)(r->count / r->channels),
            (unsigned long)r->dropped, (unsigned long)r->underruns);
    }
    api_buf_add(b, "]}", 2);
}

static void _le(api_buf_t *b, uint32_t v, size_t bytes) {
    uint8_t out[4] = { v, v >> 8, v >> 16, v >> 24 };
    api_buf_add(b, (const char *)out, bytes);
}

bool audio_capture_wav(size_t index, api_buf_t *b) {
    if (index >= _count) {
        return false;
    }
    const recording_t *r = &_rec[index];
    uint32_t data = (uint32_t)(r->count * sizeof(int16_t));
    api_buf_add(b, "RIFF", 4);
    _le(b, 36 + data, 4);
    api_buf_add(b, "WAVEfmt ", 8);
    _le(b, 16, 4);
    _le(b, 1, 2);                                   // PCM
    _le(b, r->channels, 2);
    _le(b, r->rate, 4);
    _le(b, r->rate * r->channels * 2, 4);
    _le(b, r->channels * 2, 2);
    _le(b, 16, 2);
    api_buf_add(b, "data", 4);
    _le(b, data, 4);
    // Little-endian, as every host this builds for is.
    api_buf_add(b, (const char *)r->samples, data);
    return true;
}
