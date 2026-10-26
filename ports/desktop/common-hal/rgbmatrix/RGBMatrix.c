// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "common-hal/rgbmatrix/RGBMatrix.h"

static int _timer;

void *common_hal_rgbmatrix_timer_allocate(rgbmatrix_rgbmatrix_obj_t *self) {
    (void)self;
    return &_timer;
}

void common_hal_rgbmatrix_timer_enable(void *ptr) {
    (void)ptr;
}

void common_hal_rgbmatrix_timer_disable(void *ptr) {
    (void)ptr;
}

void common_hal_rgbmatrix_timer_free(void *ptr) {
    (void)ptr;
}
