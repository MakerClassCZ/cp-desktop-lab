// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <SDL.h>
#include <string.h>

#include "py/runtime.h"
#include "common-hal/audioio/AudioOut.h"
#include "shared-bindings/audioio/AudioOut.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-module/audiocore/__init__.h"
#include "shared/runtime/interrupt_char.h"
#include "supervisor/shared/tick.h"

#include "desktop_port.h"
#include "sdl_host.h"

#define QUEUE_MS 40
#define MAX_OUTS 4

static audioio_audioout_obj_t *_outs[MAX_OUTS];

void common_hal_audioio_audioout_construct(audioio_audioout_obj_t *self,
    const mcu_pin_obj_t *left_channel, const mcu_pin_obj_t *right_channel, uint16_t default_value) {
    if (!desktop_options.fast_time && !desktop_sdl_init(SDL_INIT_AUDIO)) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("No audio"));
    }
    int slot = 0;
    while (slot < MAX_OUTS && _outs[slot] != NULL) {
        slot++;
    }
    if (slot == MAX_OUTS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("All channels in use"));
    }
    common_hal_mcu_pin_claim(left_channel);
    self->left_channel = left_channel;
    self->sample = MP_OBJ_NULL;
    self->device = 0;
    self->playing = false;
    self->paused = false;
    _outs[slot] = self;
}

bool common_hal_audioio_audioout_deinited(audioio_audioout_obj_t *self) {
    return self->left_channel == NULL;
}

void common_hal_audioio_audioout_deinit(audioio_audioout_obj_t *self) {
    if (common_hal_audioio_audioout_deinited(self)) {
        return;
    }
    common_hal_audioio_audioout_stop(self);
    for (int i = 0; i < MAX_OUTS; i++) {
        if (_outs[i] == self) {
            _outs[i] = NULL;
        }
    }
    common_hal_reset_pin(self->left_channel);
    self->left_channel = NULL;
}

// Convert one buffer to signed 16-bit and queue it; returns the bytes queued.
static uint32_t _queue(audioio_audioout_obj_t *self, const uint8_t *data, uint32_t length) {
    int16_t out[1024];
    uint32_t bytes = self->bits_per_sample / 8;
    uint32_t count = length / bytes;
    uint32_t queued = 0;
    uint16_t peak = 0;
    for (uint32_t done = 0; done < count;) {
        uint32_t n = count - done < MP_ARRAY_SIZE(out) ? count - done : MP_ARRAY_SIZE(out);
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *p = data + (done + i) * bytes;
            if (bytes == 2) {
                uint16_t v;
                memcpy(&v, p, sizeof(v));
                out[i] = self->samples_signed ? (int16_t)v : (int16_t)(v ^ 0x8000);
            } else {
                out[i] = self->samples_signed ? (int16_t)((int8_t)*p * 256) : (int16_t)((*p - 128) * 256);
            }
            uint16_t level = (uint16_t)(out[i] < 0 ? -(int32_t)out[i] : out[i]);
            peak = level > peak ? level : peak;
        }
        if (self->device != 0) {
            SDL_QueueAudio(self->device, out, n * sizeof(int16_t));
        }
        queued += n * sizeof(int16_t);
        done += n;
    }
    self->peak = peak > 32767 ? 32767 : peak;
    return queued;
}

// Fast time: the frames the program's clock has played since play.
static uint64_t _fast_due(audioio_audioout_obj_t *self) {
    return (supervisor_ticks_ms64() - self->fast_start_ms) * self->sample_rate / 1000;
}

static void _fill(audioio_audioout_obj_t *self) {
    uint32_t frame = self->channel_count * sizeof(int16_t);
    int64_t target = self->sample_rate * frame * QUEUE_MS / 1000;
    int64_t queued;
    if (desktop_options.fast_time) {
        // Behind after a skipped sleep: a sample takes up what the program's clock played, a
        // loop drops its skipped turns, which would only be made to be thrown away.
        uint64_t due = _fast_due(self);
        if (self->loop && self->fast_frames < due) {
            self->fast_frames = due;
        }
        queued = ((int64_t)self->fast_frames - (int64_t)due) * frame;
    } else {
        queued = SDL_GetQueuedAudioSize(self->device);
    }
    while (!self->sample_done && queued < target && !mp_hal_is_interrupted()) {
        uint8_t *buffer;
        uint32_t length;
        audioio_get_buffer_result_t result = audiosample_get_buffer(self->sample, false, 0, &buffer, &length);
        if (result == GET_BUFFER_ERROR) {
            self->sample_done = true;
            break;
        }
        uint32_t bytes = _queue(self, buffer, length);
        queued += bytes;
        self->fast_frames += bytes / frame;
        if (result == GET_BUFFER_DONE) {
            if (self->loop) {
                audiosample_reset_buffer(self->sample, false, 0);
            } else {
                self->sample_done = true;
            }
        }
    }
}

void common_hal_audioio_audioout_play(audioio_audioout_obj_t *self, mp_obj_t sample, bool loop) {
    common_hal_audioio_audioout_stop(self);
    audiosample_base_t *base = audiosample_check(sample);
    bool single_buffer, samples_signed;
    uint32_t max_buffer_length;
    uint8_t spacing;
    audiosample_get_buffer_structure(base, false, &single_buffer, &samples_signed, &max_buffer_length, &spacing);
    self->sample_rate = audiosample_get_sample_rate(base);
    self->channel_count = audiosample_get_channel_count(base);
    self->bits_per_sample = audiosample_get_bits_per_sample(base);
    self->samples_signed = samples_signed;

    SDL_AudioSpec want = {0};
    want.freq = self->sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = self->channel_count;
    want.samples = 512;
    self->device = 0;
    if (!desktop_options.fast_time) {
        self->device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);    // SDL converts to the hardware
        if (self->device == 0) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("No audio"));
        }
    }
    self->fast_start_ms = supervisor_ticks_ms64();
    self->fast_frames = 0;
    audiosample_reset_buffer(sample, false, 0);
    self->sample = sample;
    self->loop = loop;
    self->sample_done = false;
    self->paused = false;
    self->playing = true;
    _fill(self);
    if (self->device != 0) {
        SDL_PauseAudioDevice(self->device, 0);
    }
    // The queue is topped up from the background task, which runs with the tick.
    supervisor_enable_tick();
}

void common_hal_audioio_audioout_stop(audioio_audioout_obj_t *self) {
    if (self->device != 0) {
        SDL_CloseAudioDevice(self->device);
        self->device = 0;
    }
    if (self->playing) {
        self->playing = false;
        supervisor_disable_tick();
    }
    self->sample = MP_OBJ_NULL;
    self->paused = false;
}

bool common_hal_audioio_audioout_get_playing(audioio_audioout_obj_t *self) {
    return self->playing;
}

void common_hal_audioio_audioout_pause(audioio_audioout_obj_t *self) {
    if (!self->playing || self->paused) {
        return;
    }
    if (self->device != 0) {
        SDL_PauseAudioDevice(self->device, 1);
    } else {
        self->fast_start_ms = supervisor_ticks_ms64() - self->fast_start_ms;
    }
    self->paused = true;
}

void common_hal_audioio_audioout_resume(audioio_audioout_obj_t *self) {
    if (!self->paused) {
        return;
    }
    if (self->device != 0) {
        SDL_PauseAudioDevice(self->device, 0);
    } else {
        self->fast_start_ms = supervisor_ticks_ms64() - self->fast_start_ms;
    }
    self->paused = false;
}

bool common_hal_audioio_audioout_get_paused(audioio_audioout_obj_t *self) {
    return self->paused;
}

void audioout_background(void) {
    for (int i = 0; i < MAX_OUTS; i++) {
        audioio_audioout_obj_t *self = _outs[i];
        if (self == NULL || !self->playing || self->paused) {
            continue;
        }
        _fill(self);
        bool drained = desktop_options.fast_time ? _fast_due(self) >= self->fast_frames
            : SDL_GetQueuedAudioSize(self->device) == 0;
        if (self->sample_done && drained) {
            common_hal_audioio_audioout_stop(self);
        }
    }
}

void audioout_reset(void) {
    for (int i = 0; i < MAX_OUTS; i++) {
        if (_outs[i] != NULL) {
            common_hal_audioio_audioout_deinit(_outs[i]);
        }
    }
}
