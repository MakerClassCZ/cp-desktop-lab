// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/wifi/Monitor.h"

void common_hal_wifi_monitor_construct(wifi_monitor_obj_t *self, uint8_t channel, size_t queue) {
    (void)self;
    (void)channel;
    (void)queue;
    mp_raise_NotImplementedError(MP_ERROR_TEXT("wifi.Monitor needs a board's radio"));
}

bool common_hal_wifi_monitor_deinited(wifi_monitor_obj_t *self) {
    (void)self;
    return true;
}

void common_hal_wifi_monitor_deinit(wifi_monitor_obj_t *self) {
    (void)self;
}

void common_hal_wifi_monitor_set_channel(wifi_monitor_obj_t *self, uint8_t channel) {
    (void)self;
    (void)channel;
}

mp_obj_t common_hal_wifi_monitor_get_channel(wifi_monitor_obj_t *self) {
    (void)self;
    return MP_OBJ_NEW_SMALL_INT(0);
}

mp_obj_t common_hal_wifi_monitor_get_queue(wifi_monitor_obj_t *self) {
    (void)self;
    return MP_OBJ_NEW_SMALL_INT(0);
}

mp_obj_t common_hal_wifi_monitor_get_lost(wifi_monitor_obj_t *self) {
    (void)self;
    return MP_OBJ_NEW_SMALL_INT(0);
}

mp_obj_t common_hal_wifi_monitor_get_queued(wifi_monitor_obj_t *self) {
    (void)self;
    return MP_OBJ_NEW_SMALL_INT(0);
}

mp_obj_t common_hal_wifi_monitor_get_packet(wifi_monitor_obj_t *self) {
    (void)self;
    return mp_const_none;
}
