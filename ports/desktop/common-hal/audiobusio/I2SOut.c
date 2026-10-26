// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/audioio/AudioOut.h"
#include "shared-bindings/audiobusio/I2SOut.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-module/audiocore/__init__.h"

#include "parts.h"

void common_hal_audiobusio_i2sout_construct(audiobusio_i2sout_obj_t *self,
    const mcu_pin_obj_t *bit_clock, const mcu_pin_obj_t *word_select, const mcu_pin_obj_t *data,
    const mcu_pin_obj_t *main_clock, bool left_justified, bool external_clock) {
    (void)left_justified;
    if (main_clock != NULL || external_clock) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("main_clock and external_clock"));
    }
    part_t *part = desktop_pin_is_gpio(data->number) ? parts_for_pin(data->number, PART_SPEAKER) : NULL;
    // The data pin is the AudioOut's own; the clocks are claimed beside it.
    common_hal_audioio_audioout_construct(&self->out, data, NULL, 0);
    common_hal_mcu_pin_claim(bit_clock);
    common_hal_mcu_pin_claim(word_select);
    self->bit_clock = bit_clock;
    self->word_select = word_select;
    self->data = data;
    self->part = part;
    if (part != NULL) {
        part->out = &self->out;
        part->label = NULL;
    }
}

bool common_hal_audiobusio_i2sout_deinited(audiobusio_i2sout_obj_t *self) {
    return self->data == NULL;
}

void common_hal_audiobusio_i2sout_deinit(audiobusio_i2sout_obj_t *self) {
    if (common_hal_audiobusio_i2sout_deinited(self)) {
        return;
    }
    common_hal_audiobusio_i2sout_stop(self);
    common_hal_audioio_audioout_deinit(&self->out);
    common_hal_reset_pin(self->bit_clock);
    common_hal_reset_pin(self->word_select);
    if (self->part != NULL) {
        ((part_t *)self->part)->out = NULL;
    }
    self->part = NULL;
    self->data = NULL;
}

void common_hal_audiobusio_i2sout_play(audiobusio_i2sout_obj_t *self, mp_obj_t sample, bool loop) {
    common_hal_audioio_audioout_play(&self->out, sample, loop);
    if (self->part != NULL) {
        parts_speaker(self->part, true, self->out.sample_rate, self->out.bits_per_sample, self->out.channel_count);
    }
}

void common_hal_audiobusio_i2sout_stop(audiobusio_i2sout_obj_t *self) {
    common_hal_audioio_audioout_stop(&self->out);
    if (self->part != NULL) {
        parts_speaker(self->part, false, 0, 0, 0);
    }
}

bool common_hal_audiobusio_i2sout_get_playing(audiobusio_i2sout_obj_t *self) {
    return common_hal_audioio_audioout_get_playing(&self->out);
}

void common_hal_audiobusio_i2sout_pause(audiobusio_i2sout_obj_t *self) {
    common_hal_audioio_audioout_pause(&self->out);
}

void common_hal_audiobusio_i2sout_resume(audiobusio_i2sout_obj_t *self) {
    common_hal_audioio_audioout_resume(&self->out);
}

bool common_hal_audiobusio_i2sout_get_paused(audiobusio_i2sout_obj_t *self) {
    return common_hal_audioio_audioout_get_paused(&self->out);
}
