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

// Seesaw boards: a write is [module, function, data...], a read answers the command written
// last. The product says which of the device's channels its touch pads, thermometer, encoder
// and button read; PWM outputs and NeoPixels go to the bus trace.

static uint64_t _seesaw_pins(const uint8_t *data, size_t len) {
    uint64_t pins = 0;
    for (size_t i = 0; i < len && i < 8; i++) {
        pins |= (uint64_t)data[i] << (i < 4 ? 8 * (3 - i) : 32 + 8 * (7 - i));
    }
    return pins;
}

static void _pwm_trace(i2c_virtual_device_t *dev, const seesaw_product_t *p, int i) {
    unsigned hz = dev->pwm_hz[i] ? dev->pwm_hz[i] : 1000;
    char name[12];
    if (i < p->pwm_count) {
        snprintf(name, sizeof(name), "%s", p->pwm[i]);
    } else {
        snprintf(name, sizeof(name), "PWM%d", i);
    }
    bus_trace_text("%s %s %u/65535 at %u Hz: %.2f ms high", dev->name, name, (unsigned)dev->pwm[i], hz,
        dev->pwm[i] / 65535.0 * 1000.0 / hz);
}

static int32_t _position(i2c_virtual_device_t *dev, const seesaw_product_t *p) {
    return p->encoder_channel >= 0 ? (int32_t)lround(dev->value[p->encoder_channel]) : 0;
}

void i2c_model_seesaw_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len, const seesaw_product_t *p) {
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
            case 0x02:
                dev->gpio_output |= pins;
                break;                                      // DIRSET_BULK
            case 0x03:
                dev->gpio_output &= ~pins;
                break;                                      // DIRCLR_BULK
            case 0x05:
                dev->gpio_level |= pins;
                break;                                      // BULK_SET
            case 0x06:
                dev->gpio_level &= ~pins;
                break;                                      // BULK_CLR
            case 0x07:
                dev->gpio_level ^= pins;
                break;                                      // BULK_TOGGLE
            case 0x0b:
                dev->gpio_pull |= pins;
                break;                                      // PULLENSET
            case 0x0c:
                dev->gpio_pull &= ~pins;
                break;                                      // PULLENCLR
            default:
                break;
        }
    } else if (base == 0x08 && n >= 3 && arg[0] < 16) {     // TIMER
        uint16_t value = (uint16_t)(arg[1] << 8 | arg[2]);
        uint16_t *slot = function == 0x01 ? &dev->pwm[arg[0]] : function == 0x02 ? &dev->pwm_hz[arg[0]] : NULL;
        if (slot != NULL && *slot != value) {
            *slot = value;
            _pwm_trace(dev, p, arg[0]);
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
    } else if (base == 0x11 && function == 0x30 && n >= 4 && p->encoder_channel >= 0) {    // ENCODER_POSITION
        int32_t position = (int32_t)((uint32_t)arg[0] << 24 | (uint32_t)arg[1] << 16 | (uint32_t)arg[2] << 8 | arg[3]);
        dev->value[p->encoder_channel] = position;      // the program sets it: no panel line
        dev->encoder_read = position;
    }
}

void i2c_model_seesaw_read(i2c_virtual_device_t *dev, uint8_t *data, size_t len, const seesaw_product_t *p) {
    uint8_t answer[8] = { 0 };
    uint8_t base = dev->seesaw_base, function = dev->seesaw_function;
    uint32_t word = 0;
    bool is_word = false;
    if (base == 0x00 && function == 0x01) {                 // STATUS_HW_ID
        answer[0] = p->hw_id;
    } else if (base == 0x00 && function == 0x02) {          // STATUS_VERSION: the product, then a date
        word = (uint32_t)p->pid << 16;
        is_word = true;
    } else if (base == 0x00 && function == 0x04) {          // STATUS_TEMP, in 1/65536 C
        double c = p->temp_channel >= 0 ? dev->value[p->temp_channel] : 25;
        word = (uint32_t)(int32_t)lround(c * 65536);
        is_word = true;
    } else if (base == 0x01 && function == 0x04) {          // GPIO_BULK
        // An output reads what it drives, an input its pull, a floating one low; the button
        // pressed reads low.
        uint64_t level = dev->gpio_level & (dev->gpio_output | dev->gpio_pull);
        if (p->button_channel >= 0 && dev->value[p->button_channel] >= 0.5) {
            level &= ~((uint64_t)1 << p->button_pin);
        }
        for (int i = 0; i < 4; i++) {
            answer[i] = (uint8_t)(level >> (8 * (3 - i)));
            answer[4 + i] = (uint8_t)(level >> (32 + 8 * (3 - i)));
        }
    } else if (base == 0x0f && function >= 0x10 && function < 0x14) {   // TOUCH_CHANNEL_OFFSET
        int8_t channel = p->touch_channel[function - 0x10];
        uint16_t raw = channel >= 0 ? (uint16_t)lround(dev->value[channel]) : 0;
        answer[0] = (uint8_t)(raw >> 8);
        answer[1] = (uint8_t)raw;
    } else if (base == 0x11 && function == 0x30) {          // ENCODER_POSITION
        dev->encoder_read = _position(dev, p);
        word = (uint32_t)dev->encoder_read;
        is_word = true;
    } else if (base == 0x11 && function == 0x40) {          // ENCODER_DELTA since the last read
        int32_t now = _position(dev, p);
        word = (uint32_t)(now - dev->encoder_read);
        dev->encoder_read = now;
        is_word = true;
    }
    if (is_word) {
        for (int i = 0; i < 4; i++) {
            answer[i] = (uint8_t)(word >> (8 * (3 - i)));
        }
    }
    for (size_t i = 0; i < len; i++) {
        data[i] = i < sizeof(answer) ? answer[i] : 0;
    }
}

// ---- the products ----

// Crickit (SAMD09, product 9999): its PWM outputs in the seesaw's order
// (adafruit_seesaw.crickit.Crickit_Pinmap); touch pads about 300 untouched, adafruit_crickit
// counts one touched 100 over its first reading.
static const char *const _crickit_pwm[] = {
    "SERVO4", "SERVO3", "SERVO2", "SERVO1", "MOTOR2B", "MOTOR2A", "MOTOR1A", "MOTOR1B",
    "DRIVE4", "DRIVE3", "DRIVE2", "DRIVE1",
};
static const seesaw_product_t _crickit = {
    .hw_id = 0x55, .pid = 9999, .pwm = _crickit_pwm, .pwm_count = 12, .touch_channel = { 0, 1, 2, 3 },
    .temp_channel = -1, .encoder_channel = -1, .button_channel = -1,
};

// The rotary encoder breakout (ATtiny817, product 4991): the encoder, its push button on pin 24
// and a NeoPixel on pin 6.
static const seesaw_product_t _rotary = {
    .hw_id = 0x87, .pid = 4991, .touch_channel = { -1, -1, -1, -1 }, .temp_channel = -1,
    .encoder_channel = 0, .button_pin = 24, .button_channel = 1,
};

// The soil sensor (SAMD09, product 4026): moisture on touch pad 0, about 200 dry to 2000 wet,
// and the chip's thermometer.
static const seesaw_product_t _soil = {
    .hw_id = 0x55, .pid = 4026, .touch_channel = { 0, -1, -1, -1 }, .temp_channel = 1,
    .encoder_channel = -1, .button_channel = -1,
};

#define PRODUCT(name, product) \
    static void _##name##_write(i2c_virtual_device_t * dev, const uint8_t * data, size_t len) { \
        i2c_model_seesaw_write(dev, data, len, &product); \
    } \
    static void _##name##_read(i2c_virtual_device_t * dev, uint8_t * data, size_t len) { \
        i2c_model_seesaw_read(dev, data, len, &product); \
    }
PRODUCT(crickit, _crickit)
PRODUCT(rotary, _rotary)
PRODUCT(soil, _soil)

const i2c_virtual_model_t i2c_model_crickit = {
    "crickit", 0x49, 4, {
        { "touch1", "", 0, 1023, 300, 0 }, { "touch2", "", 0, 1023, 300, 0 }, { "touch3", "", 0, 1023, 300, 0 },
        { "touch4", "", 0, 1023, 300, 0 }
    },
    _crickit_write, _crickit_read, .driver = "adafruit_seesaw"
};

const i2c_virtual_model_t i2c_model_rotary = {
    "rotary", 0x36, 2, { { "position", "", -1000, 1000, 0, 0 }, { "button", "", 0, 1, 0, 0 } },
    _rotary_write, _rotary_read, .driver = "adafruit_seesaw"
};

const i2c_virtual_model_t i2c_model_soil = {
    "soil", 0x36, 2, { { "moisture", "", 200, 2000, 500, 0 }, { "temp", "C", -10, 50, 22, 1 } },
    _soil_write, _soil_read, .driver = "adafruit_seesaw"
};
