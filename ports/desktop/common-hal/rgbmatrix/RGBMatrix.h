// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "shared-module/rgbmatrix/RGBMatrix.h"

// The matrix is drawn on the panel, so it needs no refresh timer: these hand out a token.
void *common_hal_rgbmatrix_timer_allocate(rgbmatrix_rgbmatrix_obj_t *self);
void common_hal_rgbmatrix_timer_enable(void *);
void common_hal_rgbmatrix_timer_disable(void *);
void common_hal_rgbmatrix_timer_free(void *);
