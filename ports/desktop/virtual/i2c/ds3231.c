// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <time.h>

#include "desktop_port.h"
#include "models.h"

// DS3231: a real-time clock on the computer's, moved by what the program sets

static uint8_t _bcd(int v) {
    return (uint8_t)((v / 10) << 4 | (v % 10));
}

static int _unbcd(uint8_t v) {
    return (v >> 4) * 10 + (v & 0xf);
}

static void _ds3231_time(i2c_virtual_device_t *dev) {
    time_t now = (time_t)(desktop_program_time() + dev->clock_offset);
    struct tm *t = localtime(&now);
    dev->regs[0] = _bcd(t->tm_sec);
    dev->regs[1] = _bcd(t->tm_min);
    dev->regs[2] = _bcd(t->tm_hour);
    dev->regs[3] = (uint8_t)((t->tm_wday + dev->weekday_shift + 7) % 7 + 1);
    dev->regs[4] = _bcd(t->tm_mday);
    dev->regs[5] = _bcd(t->tm_mon + 1);
    dev->regs[6] = _bcd(t->tm_year % 100);
    dev->regs[0x11] = 25;               // 25.00 degrees
    dev->regs[0x12] = 0;
}

static void _ds3231_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    dev->pointer = data[0];
    bool time_set = false;
    _ds3231_time(dev);
    for (size_t i = 1; i < len; i++) {
        time_set |= dev->pointer <= 6;
        dev->regs[dev->pointer++] = data[i];
    }
    if (time_set) {
        // The chip keeps the weekday it is given, whatever the date.
        uint8_t weekday = dev->regs[3];
        struct tm t = { 0 };
        t.tm_sec = _unbcd(dev->regs[0]);
        t.tm_min = _unbcd(dev->regs[1]);
        t.tm_hour = _unbcd(dev->regs[2] & 0x3f);
        t.tm_mday = _unbcd(dev->regs[4]);
        t.tm_mon = _unbcd(dev->regs[5] & 0x1f) - 1;
        t.tm_year = 100 + _unbcd(dev->regs[6]);
        t.tm_isdst = -1;
        dev->clock_offset = (long long)(mktime(&t) - desktop_program_time());
        dev->weekday_shift = 0;
        _ds3231_time(dev);
        dev->weekday_shift = (weekday - 1) - (dev->regs[3] - 1);
    }
}

static void _ds3231_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    _ds3231_time(dev);
    for (size_t i = 0; i < len; i++) {
        data[i] = dev->regs[dev->pointer++];
    }
}

const i2c_virtual_model_t i2c_model_ds3231 = {
    "ds3231", 0x68, 0, { { NULL } }, _ds3231_write, _ds3231_read
};
