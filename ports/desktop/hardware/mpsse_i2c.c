// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// busio.I2C on the FT232H: the MPSSE bit-bang behind the backend's I2C ops.

#include "py/mpconfig.h"

#if CIRCUITPY_BUSIO_I2C

#include <string.h>

#include "py/mperrno.h"
#include "py/runtime.h"

#include "desktop_pins.h"
#include "mpsse.h"

// I2C by open-drain emulation (FTDI AN_113, as pyftdi does it): SCL on D0 and SDA out on D1 are
// outputs under drive-only-zero, so a "1" releases the line; SDA is read back on D2, which the
// board ties to D1. Three-phase clocking keeps data stable on the clock's high phase.
#define SCL 0x01
#define SDA_OUT 0x02
#define SDA_IN 0x04
#define I2C_DIR (SCL | SDA_OUT)
#define I2C_PINS (SCL | SDA_OUT | SDA_IN)

// One flush: at most this many command bytes, and the bytes coming back stay well under the
// chip's FIFO towards the host (the engine stalls otherwise, see mpsse.c).
#define CMD_MAX 8192
#define WRITE_BATCH 64
#define BYTE_IN_MAX (6 + 3 * 5)     // _byte_in() at the slowest timings

static uint8_t _cmd[CMD_MAX];
static size_t _cmd_len;
static bool _cmd_overflow;

// The hold, set-up and bus-free times as repeats of a line-state command.
static unsigned _hd_sta, _su_sto, _idle, _delay;

static void _timings(uint32_t frequency) {
    if (frequency <= 100000) {
        _hd_sta = 4;
        _su_sto = 4;
        _idle = 5;
        _delay = 5;
    } else if (frequency <= 400000) {
        _hd_sta = 2;
        _su_sto = 2;
        _idle = 2;
        _delay = 2;
    } else {
        _hd_sta = 1;
        _su_sto = 1;
        _idle = 1;
        _delay = 1;
    }
}

static void _put(uint8_t b) {
    if (_cmd_len < CMD_MAX) {
        _cmd[_cmd_len++] = b;
    } else {
        _cmd_overflow = true;
    }
}

// A line state, with the other pins of the bank as they are.
static void _lines(uint8_t value, unsigned repeat) {
    uint8_t others_value = mpsse_bank_value(0) & ~I2C_PINS;
    uint8_t others_dir = mpsse_bank_dir(0) & ~I2C_PINS;
    for (unsigned i = 0; i < repeat; i++) {
        _put(0x80);
        _put(value | others_value);
        _put(I2C_DIR | others_dir);
    }
}

#define IDLE (SCL | SDA_OUT)
#define DATA_LO (SCL)
#define CLK_LO_DATA_LO (0)
#define CLK_LO_DATA_HI (SDA_OUT)

static void _start(void) {
    _lines(IDLE, _delay);
    _lines(DATA_LO, _hd_sta);
    _lines(CLK_LO_DATA_LO, _hd_sta);
}

static void _stop(void) {
    _lines(CLK_LO_DATA_HI, _hd_sta);
    _lines(CLK_LO_DATA_LO, _hd_sta);
    _lines(DATA_LO, _su_sto);
    _lines(IDLE, _idle);
}

// A byte out, then SDA released and the acknowledge bit clocked in (one byte back per call).
static void _byte_out(uint8_t byte_out) {
    _put(0x11);
    _put(0x00);
    _put(0x00);
    _put(byte_out);
    _lines(CLK_LO_DATA_HI, 1);
    _put(0x22);
    _put(0x00);
}

// A byte in (one byte back), then our acknowledge or, for the last byte, none.
static void _byte_in(bool last) {
    _put(0x20);
    _put(0x00);
    _put(0x00);
    _put(0x13);
    _put(0x00);
    _put(last ? 0xFF : 0x00);
    _lines(CLK_LO_DATA_HI, _delay);
}

static bool _flush(uint8_t *reply, size_t reply_len, uint32_t frequency) {
    _put(0x87);
    bool ok = !_cmd_overflow && mpsse_send(_cmd, _cmd_len);
    _cmd_len = 0;
    _cmd_overflow = false;
    if (ok && reply_len > 0) {
        ok = mpsse_recv(reply, reply_len, 1000 + (unsigned)((uint64_t)reply_len * 10 * 1000 / frequency));
    }
    return ok;
}

static uint32_t _frequency;

static bool _construct(uint32_t frequency, uint32_t *actual_hz) {
    // Both lines released: outputs at 1 under drive-only-zero; SDA read back on D2.
    mpsse_gpio_set(0, true, true, true);
    mpsse_gpio_set(1, true, true, true);
    mpsse_gpio_set(2, false, false, false);
    mpsse_set_clock(frequency, true);
    _frequency = mpsse_clock_hz();
    *actual_hz = _frequency;
    _timings(_frequency);
    _cmd_len = 0;
    _cmd_overflow = false;
    return !mpsse_lost();
}

static void _deinit(void) {
}

// Start, the address, and the bytes out in batches; the acknowledge bits come back as one byte
// each. Returns 0, -MP_ENODEV when the address is not acknowledged, -MP_EIO when a byte is not.
static int _write(uint16_t address, bool read, const uint8_t *data, size_t len) {
    uint8_t acks[1 + WRITE_BATCH];
    _start();
    _byte_out((uint8_t)((address << 1) | (read ? 1 : 0)));
    if (!_flush(acks, 1, _frequency)) {
        return -MP_EIO;
    }
    if (acks[0] & 1) {
        return -MP_ENODEV;
    }
    size_t done = 0;
    while (done < len) {
        size_t take = len - done < WRITE_BATCH ? len - done : WRITE_BATCH;
        for (size_t i = 0; i < take; i++) {
            _byte_out(data[done + i]);
        }
        if (!_flush(acks, take, _frequency)) {
            return -MP_EIO;
        }
        for (size_t i = 0; i < take; i++) {
            if (acks[i] & 1) {
                return -MP_EIO;
            }
        }
        done += take;
    }
    return 0;
}

static int _read_bytes(uint8_t *data, size_t len) {
    const size_t batch = (CMD_MAX - 16) / BYTE_IN_MAX;
    size_t done = 0;
    while (done < len) {
        size_t take = len - done < batch ? len - done : batch;
        for (size_t i = 0; i < take; i++) {
            _byte_in(done + i + 1 == len);
        }
        if (!_flush(data + done, take, _frequency)) {
            return -MP_EIO;
        }
        done += take;
    }
    return 0;
}

static int _finish(int status) {
    _stop();
    bool ok = _flush(NULL, 0, 1);
    return status != 0 ? status : (ok ? 0 : -MP_EIO);
}

static int _op_write(uint16_t address, const uint8_t *data, size_t len, bool stop) {
    if (mpsse_lost()) {
        return -MP_EIO;
    }
    int status = _write(address, false, data, len);
    if (!stop && status == 0) {
        // The next op starts again on the idle lines: a repeated start.
        return 0;
    }
    return _finish(status);
}

static int _op_read(uint16_t address, uint8_t *data, size_t len) {
    if (mpsse_lost()) {
        return -MP_EIO;
    }
    int status = _write(address, true, NULL, 0);
    if (status == 0) {
        status = _read_bytes(data, len);
    }
    return _finish(status);
}

const desktop_i2c_ops_t mpsse_i2c_ops = {
    .scl = 0,
    .sda = 1,
    .construct = _construct,
    .deinit = _deinit,
    .write = _op_write,
    .read = _op_read,
};

#endif
