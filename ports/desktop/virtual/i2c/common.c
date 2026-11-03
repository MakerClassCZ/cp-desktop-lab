// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "models.h"

// A register pointer, then the bytes written from it on.
void i2c_model_plain_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len) {
    dev->pointer = data[0];
    for (size_t i = 1; i < len; i++) {
        dev->regs[dev->pointer++] = data[i];
    }
}

// The same for ST's sensors, whose pointer's bit 7 asks for the auto-increment they always do,
// leaving a self-clearing reset bit clear: LIS3DH CTRL5 bit 7, LSM6DS CTRL3_C bit 0, LIS3MDL
// CTRL_REG2 bit 2.
void i2c_model_st_write(i2c_virtual_device_t *dev, const uint8_t *data, size_t len, uint8_t reset_reg, uint8_t reset_bit) {
    dev->pointer = data[0] & 0x7f;
    for (size_t i = 1; i < len; i++) {
        dev->regs[dev->pointer] = data[i];
        if (dev->pointer == reset_reg) {
            dev->regs[reset_reg] &= (uint8_t) ~reset_bit;
        }
        dev->pointer = (dev->pointer + 1) & 0x7f;
    }
}

// Sensirion's (SHT, SCD): CRC-8, polynomial 0x31, starting at 0xff, over each word.
void i2c_model_sensirion_word(uint8_t *out, uint16_t word) {
    uint8_t crc = 0xff;
    out[0] = (uint8_t)(word >> 8);
    out[1] = (uint8_t)word;
    for (int k = 0; k < 2; k++) {
        crc ^= out[k];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    out[2] = crc;
}
