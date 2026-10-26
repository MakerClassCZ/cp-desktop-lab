// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bus_trace.h"
#include "models.h"

// Crickit: a seesaw (SAMD09, product 9999); a write is [module, function, data...], a read
// answers the command written last. Its touch pads are the channels; servos, motors, drives and
// NeoPixels go to the bus trace. ----

// The Crickit's PWM outputs in the seesaw's order (adafruit_seesaw.crickit.Crickit_Pinmap).
static const char *const _crickit_pwm[] = {
    "SERVO4", "SERVO3", "SERVO2", "SERVO1", "MOTOR2B", "MOTOR2A", "MOTOR1A", "MOTOR1B",
    "DRIVE4", "DRIVE3", "DRIVE2", "DRIVE1",
};

static uint64_t _seesaw_pins(const uint8_t *data, size_t len) {
    uint64_t pins = 0;
    for (size_t i = 0; i < len && i < 8; i++) {
        pins |= (uint64_t)data[i] << (i < 4 ? 8 * (3 - i) : 32 + 8 * (7 - i));
    }
    return pins;
}

static void _crickit_pwm_trace(i2c_virtual_device_t *dev, int i) {
    unsigned hz = dev->pwm_hz[i] ? dev->pwm_hz[i] : 1000;
    bus_trace_text("%s %s %u/65535 at %u Hz: %.2f ms high", dev->name, _crickit_pwm[i], (unsigned)dev->pwm[i], hz,
        dev->pwm[i] / 65535.0 * 1000.0 / hz);
}

static void _crickit_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    if (len < 2) {
        return;
    }
    uint8_t base = data[0], function = data[1];
    const uint8_t *arg = data + 2;
    size_t n = len - 2;
    dev->seesaw_base = base;
    dev->seesaw_function = function;
    if (base == 0x00 && function == 0x7f) {             // STATUS_SWRST
        dev->gpio_output = dev->gpio_level = dev->gpio_pull = 0;
        memset(dev->pwm, 0, sizeof(dev->pwm));
        memset(dev->pwm_hz, 0, sizeof(dev->pwm_hz));
    } else if (base == 0x01 && n > 0) {                 // GPIO
        uint64_t pins = _seesaw_pins(arg, n);
        switch (function) {
            case 0x02: dev->gpio_output |= pins; break;     // DIRSET_BULK
            case 0x03: dev->gpio_output &= ~pins; break;    // DIRCLR_BULK
            case 0x05: dev->gpio_level |= pins; break;      // BULK_SET
            case 0x06: dev->gpio_level &= ~pins; break;     // BULK_CLR
            case 0x07: dev->gpio_level ^= pins; break;      // BULK_TOGGLE
            case 0x0b: dev->gpio_pull |= pins; break;       // PULLENSET
            case 0x0c: dev->gpio_pull &= ~pins; break;      // PULLENCLR
            default: break;
        }
    } else if (base == 0x08 && n >= 3 && arg[0] < sizeof(_crickit_pwm) / sizeof(_crickit_pwm[0])) {    // TIMER
        uint16_t value = (uint16_t)(arg[1] << 8 | arg[2]);
        uint16_t *slot = function == 0x01 ? &dev->pwm[arg[0]] : function == 0x02 ? &dev->pwm_hz[arg[0]] : NULL;
        if (slot != NULL && *slot != value) {
            *slot = value;
            _crickit_pwm_trace(dev, arg[0]);
        }
    } else if (base == 0x0e && function == 0x03 && n >= 2) {     // NEOPIXEL_BUF_LENGTH, in bytes
        dev->words[0] = (uint16_t)(arg[0] << 8 | arg[1]);
    } else if (base == 0x0e && function == 0x04 && n >= 2) {     // NEOPIXEL_BUF: an offset, then GRB
        size_t offset = (size_t)(arg[0] << 8 | arg[1]);
        for (size_t i = 2; i < n && offset + i - 2 < sizeof(dev->regs); i++) {
            dev->regs[offset + i - 2] = arg[i];
        }
    } else if (base == 0x0e && function == 0x05) {               // NEOPIXEL_SHOW
        char text[200];
        size_t count = dev->words[0] / 3, at = 0;
        at += snprintf(text, sizeof(text), "%s NEOPIXEL %u:", dev->name, (unsigned)count);
        for (size_t i = 0; i < count && 3 * i + 2 < sizeof(dev->regs) && at < sizeof(text) - 8; i++) {
            at += snprintf(text + at, sizeof(text) - at, " %02x%02x%02x", dev->regs[3 * i + 1], dev->regs[3 * i],
                dev->regs[3 * i + 2]);
        }
        bus_trace_text("%s", text);
    }
}

static void _crickit_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len) {
    uint8_t answer[8] = { 0 };
    uint8_t base = dev->seesaw_base, function = dev->seesaw_function;
    if (base == 0x00 && function == 0x01) {                 // STATUS_HW_ID: a SAMD09
        answer[0] = 0x55;
    } else if (base == 0x00 && function == 0x02) {          // STATUS_VERSION: product 9999
        answer[0] = 9999 >> 8;
        answer[1] = 9999 & 0xff;
    } else if (base == 0x00 && function == 0x04) {          // STATUS_TEMP: 25 C in 1/65536
        answer[1] = 25;
    } else if (base == 0x01 && function == 0x04) {          // GPIO_BULK
        // An output reads what it drives, an input its pull, a floating one low.
        uint64_t level = dev->gpio_level & (dev->gpio_output | dev->gpio_pull);
        for (int i = 0; i < 4; i++) {
            answer[i] = (uint8_t)(level >> (8 * (3 - i)));
            answer[4 + i] = (uint8_t)(level >> (32 + 8 * (3 - i)));
        }
    } else if (base == 0x0f && function >= 0x10 && function < 0x14) {   // TOUCH_CHANNEL_OFFSET
        uint16_t raw = (uint16_t)lround(dev->value[function - 0x10]);
        answer[0] = (uint8_t)(raw >> 8);
        answer[1] = (uint8_t)raw;
    }
    for (size_t i = 0; i < len; i++) {
        data[i] = i < sizeof(answer) ? answer[i] : 0;
    }
}

// Touch pads: about 300 untouched; adafruit_crickit counts one touched 100 over its first reading.
const i2c_virtual_model_t i2c_model_crickit = {
    "crickit", 0x49, 4, {
        { "touch1", "", 0, 1023, 300, 0 }, { "touch2", "", 0, 1023, 300, 0 }, { "touch3", "", 0, 1023, 300, 0 },
        { "touch4", "", 0, 1023, 300, 0 } },
    _crickit_write, _crickit_read
};
