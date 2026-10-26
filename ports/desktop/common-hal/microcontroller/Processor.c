// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common-hal/microcontroller/Processor.h"
#include "shared-bindings/microcontroller/Processor.h"
#include "shared-bindings/microcontroller/ResetReason.h"
#include "desktop_port.h"

float common_hal_mcu_processor_get_temperature(void) {
    // The kernel reports the first thermal zone in millidegrees.
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    long millideg;
    float t = NAN;
    if (f != NULL) {
        if (fscanf(f, "%ld", &millideg) == 1) {
            t = millideg / 1000.0f;
        }
        fclose(f);
    }
    return t;
}

float common_hal_mcu_processor_get_voltage(void) {
    return NAN;
}

uint32_t common_hal_mcu_processor_get_frequency(void) {
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    unsigned long khz = 0;
    if (f != NULL) {
        if (fscanf(f, "%lu", &khz) != 1) {
            khz = 0;
        }
        fclose(f);
    }
    return khz * 1000;
}

// The machine id: 32 hex digits that stay the same across reboots.
void common_hal_mcu_processor_get_uid(uint8_t raw_id[]) {
    memset(raw_id, 0, COMMON_HAL_MCU_PROCESSOR_UID_LENGTH);
    FILE *f = fopen("/etc/machine-id", "r");
    if (f == NULL) {
        return;
    }
    for (int i = 0; i < COMMON_HAL_MCU_PROCESSOR_UID_LENGTH; i++) {
        unsigned int value;
        if (fscanf(f, "%2x", &value) != 1) {
            break;
        }
        raw_id[i] = value;
    }
    fclose(f);
}

mcu_reset_reason_t common_hal_mcu_processor_get_reset_reason(void) {
    return desktop_was_reset ? MCU_RESET_REASON_SOFTWARE : MCU_RESET_REASON_POWER_ON;
}
