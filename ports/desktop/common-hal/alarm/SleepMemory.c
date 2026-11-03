// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "shared-bindings/alarm/SleepMemory.h"

uint8_t desktop_sleep_memory[DESKTOP_SLEEP_MEMORY_SIZE];

uint32_t common_hal_alarm_sleep_memory_get_length(alarm_sleep_memory_obj_t *self) {
    return sizeof(desktop_sleep_memory);
}

bool common_hal_alarm_sleep_memory_set_bytes(alarm_sleep_memory_obj_t *self, uint32_t start_index, const uint8_t *values, uint32_t len) {
    if (start_index + len > sizeof(desktop_sleep_memory)) {
        return false;
    }
    memcpy(desktop_sleep_memory + start_index, values, len);
    return true;
}

void common_hal_alarm_sleep_memory_get_bytes(alarm_sleep_memory_obj_t *self, uint32_t start_index, uint8_t *values, uint32_t len) {
    if (start_index + len > sizeof(desktop_sleep_memory)) {
        return;
    }
    memcpy(values, desktop_sleep_memory + start_index, len);
}
