// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The host's clock, local time. Setting it moves an offset; the system clock is left alone.

#include <time.h>

#include "py/obj.h"
#include "shared/timeutils/timeutils.h"
#include "shared-bindings/rtc/RTC.h"

#include "desktop_port.h"

static int64_t _offset;

void common_hal_rtc_get_time(timeutils_struct_time_t *tm) {
    time_t now = (time_t)(desktop_program_time() + _offset);
    struct tm local;
    #ifdef _WIN32
    localtime_s(&local, &now);
    #else
    localtime_r(&now, &local);
    #endif
    tm->tm_year = local.tm_year + 1900;
    tm->tm_mon = local.tm_mon + 1;
    tm->tm_mday = local.tm_mday;
    tm->tm_hour = local.tm_hour;
    tm->tm_min = local.tm_min;
    tm->tm_sec = local.tm_sec;
    tm->tm_wday = (local.tm_wday + 6) % 7;      // CircuitPython counts Monday as 0
    tm->tm_yday = local.tm_yday + 1;
}

void common_hal_rtc_set_time(timeutils_struct_time_t *tm) {
    struct tm local = {
        .tm_year = tm->tm_year - 1900,
        .tm_mon = tm->tm_mon - 1,
        .tm_mday = tm->tm_mday,
        .tm_hour = tm->tm_hour,
        .tm_min = tm->tm_min,
        .tm_sec = tm->tm_sec,
        .tm_isdst = -1,
    };
    _offset = (int64_t)mktime(&local) - desktop_program_time();
}

int common_hal_rtc_get_calibration(void) {
    return 0;
}

void common_hal_rtc_set_calibration(int calibration) {
}
