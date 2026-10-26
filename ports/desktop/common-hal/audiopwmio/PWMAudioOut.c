// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/audioio/AudioOut.h"
#include "shared-bindings/audiopwmio/PWMAudioOut.h"

#include "parts.h"

void common_hal_audiopwmio_pwmaudioout_construct(audiopwmio_pwmaudioout_obj_t *self,
    const mcu_pin_obj_t *left_channel, const mcu_pin_obj_t *right_channel, uint16_t default_value) {
    part_t *part = desktop_pin_is_gpio(left_channel->number) ? parts_for_pin(left_channel->number, PART_SPEAKER) : NULL;
    common_hal_audioio_audioout_construct(&self->out, left_channel, right_channel, default_value);
    self->constructed = true;
    self->part = part;
    if (part != NULL) {
        part->out = &self->out;
        part->label = "PWMAUDIO";
    }
}

bool common_hal_audiopwmio_pwmaudioout_deinited(audiopwmio_pwmaudioout_obj_t *self) {
    return !self->constructed;
}

void common_hal_audiopwmio_pwmaudioout_deinit(audiopwmio_pwmaudioout_obj_t *self) {
    if (common_hal_audiopwmio_pwmaudioout_deinited(self)) {
        return;
    }
    common_hal_audiopwmio_pwmaudioout_stop(self);
    common_hal_audioio_audioout_deinit(&self->out);
    if (self->part != NULL) {
        ((part_t *)self->part)->out = NULL;
    }
    self->part = NULL;
    self->constructed = false;
}

void common_hal_audiopwmio_pwmaudioout_play(audiopwmio_pwmaudioout_obj_t *self, mp_obj_t sample, bool loop) {
    common_hal_audioio_audioout_play(&self->out, sample, loop);
    if (self->part != NULL) {
        parts_speaker(self->part, true, self->out.sample_rate, self->out.bits_per_sample, self->out.channel_count);
    }
}

void common_hal_audiopwmio_pwmaudioout_stop(audiopwmio_pwmaudioout_obj_t *self) {
    common_hal_audioio_audioout_stop(&self->out);
    if (self->part != NULL) {
        parts_speaker(self->part, false, 0, 0, 0);
    }
}

bool common_hal_audiopwmio_pwmaudioout_get_playing(audiopwmio_pwmaudioout_obj_t *self) {
    return common_hal_audioio_audioout_get_playing(&self->out);
}

void common_hal_audiopwmio_pwmaudioout_pause(audiopwmio_pwmaudioout_obj_t *self) {
    common_hal_audioio_audioout_pause(&self->out);
}

void common_hal_audiopwmio_pwmaudioout_resume(audiopwmio_pwmaudioout_obj_t *self) {
    common_hal_audioio_audioout_resume(&self->out);
}

bool common_hal_audiopwmio_pwmaudioout_get_paused(audiopwmio_pwmaudioout_obj_t *self) {
    return common_hal_audioio_audioout_get_paused(&self->out);
}
