// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api_json.h"

// What the audio outputs play (AudioOut, I2SOut, PWMAudioOut), as signed 16-bit samples: one
// recording per output pin, kept across runs until cleared. On with --audio-capture or from the
// API; everything on the VM thread.

void audio_capture_start(void);     // clears the recordings and records from now on
void audio_capture_stop(void);
bool audio_capture_on(void);
// count samples (frames times channels) that the output on pin played at rate.
void audio_capture_add(uint8_t pin, uint32_t rate, uint8_t channels, const int16_t *samples, size_t count);
// The output on pin ran dry while it had more to play: a gap a listener hears.
void audio_capture_underrun(uint8_t pin);
// {"on": bool, "recordings": [{"index", "pin", "rate", "channels", "frames", "dropped", "underruns"}]}
void audio_capture_json(api_buf_t *b);
// A recording as a WAV file; false when there is no such one.
bool audio_capture_wav(size_t index, api_buf_t *b);
