// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <SDL.h>
#include <stdlib.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "shared-bindings/audiobusio/PDMIn.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "parts.h"
#include "sdl_host.h"

#define HOST_WAIT_MS 2000           // a microphone that sends nothing for this long ends a record

void common_hal_audiobusio_pdmin_construct(audiobusio_pdmin_obj_t *self,
    const mcu_pin_obj_t *clock_pin, const mcu_pin_obj_t *data_pin,
    uint32_t sample_rate, uint8_t bit_depth, bool mono, uint8_t oversample) {
    (void)oversample;
    if (!mono) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("stereo"));
    }
    if (!desktop_pin_is_gpio(data_pin->number) || sample_rate == 0) {
        raise_ValueError_invalid_pin();
    }
    part_t *part = parts_for_pin(data_pin->number, PART_MIC);
    if (part == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("too many parts on the board panel"));
    }
    self->device = 0;
    if (part->source == MIC_HOST) {
        if (!desktop_sdl_init(SDL_INIT_AUDIO)) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("No audio"));
        }
        SDL_AudioSpec want = { 0 };
        want.freq = (int)sample_rate;
        want.format = AUDIO_U16SYS;
        want.channels = 1;
        want.samples = 512;
        self->device = SDL_OpenAudioDevice(NULL, 1, &want, NULL, 0);    // SDL converts from the hardware
        if (self->device == 0) {
            mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("no microphone: %s"), SDL_GetError());
        }
        SDL_PauseAudioDevice(self->device, 0);
    }
    common_hal_mcu_pin_claim(clock_pin);
    common_hal_mcu_pin_claim(data_pin);
    self->clock = clock_pin;
    self->data = data_pin;
    self->sample_rate = sample_rate;
    self->bit_depth = bit_depth;
    self->part = part;
}

bool common_hal_audiobusio_pdmin_deinited(audiobusio_pdmin_obj_t *self) {
    return self->data == NULL;
}

void common_hal_audiobusio_pdmin_deinit(audiobusio_pdmin_obj_t *self) {
    if (common_hal_audiobusio_pdmin_deinited(self)) {
        return;
    }
    if (self->device != 0) {
        SDL_CloseAudioDevice(self->device);
        self->device = 0;
    }
    common_hal_reset_pin(self->clock);
    common_hal_reset_pin(self->data);
    self->data = NULL;
    self->part = NULL;
}

static void _put(audiobusio_pdmin_obj_t *self, void *buffer, uint32_t i, uint16_t v) {
    if (self->bit_depth == 8) {
        ((uint8_t *)buffer)[i] = (uint8_t)(v >> 8);
    } else {
        ((uint16_t *)buffer)[i] = v;
    }
}

// The computer's microphone: what it has, waiting for the rest.
static uint32_t _record_host(audiobusio_pdmin_obj_t *self, void *buffer, uint32_t length) {
    uint32_t got = 0;
    uint64_t last = mp_hal_ticks_ms();
    while (got < length && mp_hal_ticks_ms() - last < HOST_WAIT_MS) {
        uint16_t chunk[256];
        uint32_t want = length - got < 256 ? length - got : 256;
        uint32_t n = SDL_DequeueAudio(self->device, chunk, want * sizeof(uint16_t)) / sizeof(uint16_t);
        for (uint32_t i = 0; i < n; i++) {
            _put(self, buffer, got + i, chunk[i]);
        }
        got += n;
        if (n > 0) {
            last = mp_hal_ticks_ms();
        } else {
            mp_hal_delay_ms(1);
        }
    }
    return got;
}

// The generator, in real time: a record takes as long as its samples last.
static uint32_t _record_generated(audiobusio_pdmin_obj_t *self, void *buffer, uint32_t length) {
    part_t *part = self->part;
    uint64_t start = mp_hal_ticks_ms();
    double amplitude = (double)part->value / (double)2;
    double step = (double)part->tone_hz / (double)self->sample_rate;
    for (uint32_t i = 0; i < length; i++) {
        double wave;
        if (part->source == MIC_NOISE) {
            wave = (double)(rand() % 20001 - 10000) / (double)10000;
        } else {
            wave = SDL_sin(part->phase * (double)2 * (double)M_PI);
            part->phase += step;
            if (part->phase >= (double)1) {
                part->phase -= (double)1;
            }
        }
        int32_t v = 32768 + (int32_t)(wave * amplitude);
        _put(self, buffer, i, (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v));
    }
    uint64_t duration = (uint64_t)length * 1000 / self->sample_rate;
    uint64_t spent = mp_hal_ticks_ms() - start;
    if (spent < duration) {
        mp_hal_delay_ms((mp_uint_t)(duration - spent));
    }
    return length;
}

uint32_t common_hal_audiobusio_pdmin_record_to_buffer(audiobusio_pdmin_obj_t *self, uint16_t *buffer, uint32_t length) {
    return self->device != 0 ? _record_host(self, buffer, length) : _record_generated(self, buffer, length);
}

uint8_t common_hal_audiobusio_pdmin_get_bit_depth(audiobusio_pdmin_obj_t *self) {
    return self->bit_depth;
}

uint32_t common_hal_audiobusio_pdmin_get_sample_rate(audiobusio_pdmin_obj_t *self) {
    return self->sample_rate;
}
