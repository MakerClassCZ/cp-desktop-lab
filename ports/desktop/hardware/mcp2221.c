// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "mcp2221.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "py/mperrno.h"
#include "py/runtime.h"
#include "shared-bindings/digitalio/Pull.h"

#include "hid_host.h"
#include "desktop_port.h"

#define MCP_VID 0x04D8
#define MCP_PID 0x00DD

// The HID commands (MCP2221A datasheet, chapter 3).
#define CMD_STATUS 0x10
#define CMD_READ_DATA 0x40
#define CMD_SET_GPIO 0x50
#define CMD_GET_GPIO 0x51
#define CMD_SET_SRAM 0x60
#define CMD_GET_SRAM 0x61
#define CMD_I2C_WRITE 0x90
#define CMD_I2C_READ 0x91
#define CMD_I2C_READ_REPEATED 0x93
#define CMD_I2C_WRITE_NO_STOP 0x94
#define CMD_RESET_CHIP 0x70

#define I2C_STATE_IDLE 0x00
#define I2C_STATE_ADDRESS_NACK 0x25
#define I2C_STATE_WAIT_RESTART 0x45     // a write without stop is done, the engine holds the bus
#define I2C_CHUNK 60
#define NOT_GPIO 0xEE

#define XFER_TIMEOUT_MS 1000

static bool _open;
static bool _lost;
static uint8_t _dir;            // GP0-GP3 as bits: 1 = output
static uint8_t _val;
static uint8_t _desig[4];       // GPIO, ADC or DAC
static uint8_t _dac;            // the DAC's 5-bit value
static uint32_t _i2c_hz;
static bool _i2c_no_stop;       // the last write left the bus for a repeated start

bool mcp2221_lost(void) {
    return _lost;
}

static bool _write_sram_all(void);

// One command and its answer; the chip echoes the command byte. A transport failure marks the
// device lost and says why once.
static bool _xfer(const uint8_t *cmd, size_t len, uint8_t reply[HID_REPORT_SIZE]) {
    if (_lost) {
        return false;
    }
    uint8_t out[HID_REPORT_SIZE] = { 0 };
    memcpy(out, cmd, len);
    if (!hid_host_exchange(out, reply, XFER_TIMEOUT_MS) || reply[0] != cmd[0]) {
        fprintf(stderr, "circuitpython: MCP2221 lost: %s\n", reply[0] != cmd[0] ? "wrong answer" : hid_host_last_error());
        _lost = true;
        return false;
    }
    return true;
}

// The I2C engine can get stuck (state 0x62, seen after a device was plugged onto the live bus);
// cancelling does not clear it, only the chip's reset does. The chip drops off USB and comes
// back within a second with SRAM at its defaults, so the pins are set up again.
static bool _reset_chip(void) {
    fprintf(stderr, "circuitpython: MCP2221 I2C engine stuck, resetting the chip\n");
    uint8_t cmd[HID_REPORT_SIZE] = { CMD_RESET_CHIP, 0xAB, 0xCD, 0xEF };
    hid_host_send(cmd);
    hid_host_close();
    char err[128];
    for (int i = 0; i < 20; i++) {
        desktop_host_sleep_ms(250);
        if (hid_host_open(MCP_VID, MCP_PID, "MCP2221", err, sizeof(err))) {
            return _write_sram_all();
        }
    }
    fprintf(stderr, "circuitpython: %s\n", err);
    _lost = true;
    return false;
}

// Status with a cancelled transfer; false when the engine stays stuck even after a chip reset.
static bool _engine_idle(void) {
    uint8_t cancel[] = { CMD_STATUS, 0, 0x10 };
    uint8_t status[] = { CMD_STATUS };
    uint8_t reply[HID_REPORT_SIZE];
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!_xfer(cancel, sizeof(cancel), reply) || !_xfer(status, sizeof(status), reply)) {
            return false;
        }
        if (reply[8] == I2C_STATE_IDLE) {
            return true;
        }
        if (attempt == 0 && !_reset_chip()) {
            return false;
        }
    }
    return false;
}

// The four GP settings: level, direction and function, as the SRAM command wants them.
static bool _write_sram_gp(void) {
    uint8_t cmd[12] = { CMD_SET_SRAM, 0, 0, 0, 0, 0, 0, 0x80 };
    for (uint8_t gp = 0; gp < 4; gp++) {
        bool output = (_dir >> gp) & 1;
        bool level = (_val >> gp) & 1;
        cmd[8 + gp] = (uint8_t)((level ? 0x10 : 0) | (output ? 0 : 0x08) | (_desig[gp] & 7));
    }
    uint8_t reply[HID_REPORT_SIZE];
    return _xfer(cmd, sizeof(cmd), reply);
}

// The DAC and ADC on VDD, the DAC's value and the four GP settings.
static bool _write_sram_all(void) {
    uint8_t cmd[12] = { CMD_SET_SRAM, 0, 0, 0x80, (uint8_t)(0x80 | _dac), 0x80, 0, 0x80 };
    for (uint8_t gp = 0; gp < 4; gp++) {
        bool output = (_dir >> gp) & 1;
        bool level = (_val >> gp) & 1;
        cmd[8 + gp] = (uint8_t)((level ? 0x10 : 0) | (output ? 0 : 0x08) | (_desig[gp] & 7));
    }
    uint8_t reply[HID_REPORT_SIZE];
    return _xfer(cmd, sizeof(cmd), reply);
}

static bool _write_gpio(uint8_t gp) {
    uint8_t cmd[18] = { CMD_SET_GPIO, 0 };
    cmd[2 + 4 * gp] = 1;
    cmd[3 + 4 * gp] = (_val >> gp) & 1;
    cmd[4 + 4 * gp] = 1;
    cmd[5 + 4 * gp] = (_dir >> gp) & 1 ? 0 : 1;
    uint8_t reply[HID_REPORT_SIZE];
    return _xfer(cmd, sizeof(cmd), reply);
}

static bool _pin_ok(uint8_t pin) {
    return pin >= MCP2221_PIN_GP0 && pin < MCP2221_PIN_COUNT;
}

static bool _backend_present(void) {
    return hid_host_present(MCP_VID, MCP_PID);
}

static bool _backend_open(void) {
    if (_open) {
        return true;
    }
    char err[256];
    if (!hid_host_open(MCP_VID, MCP_PID, "MCP2221", err, sizeof(err))) {
        fprintf(stderr, "circuitpython: %s\n", err);
        #if !defined(_WIN32) && !defined(__APPLE__)
        fprintf(stderr, "  udev rule: KERNEL==\"hidraw*\", ATTRS{idVendor}==\"04d8\", ATTRS{idProduct}==\"00dd\", MODE=\"0666\"\n");
        #endif
        return false;
    }
    _lost = false;
    _dir = 0;
    _val = 0;
    memset(_desig, MCP2221_GPIO, sizeof(_desig));
    _dac = 0;
    _i2c_no_stop = false;
    // An idle I2C engine (a crashed run's transfer cancelled, a stuck engine reset), then every
    // GP a GPIO input with the DAC and ADC on VDD.
    if (!_engine_idle() || !_write_sram_all()) {
        fprintf(stderr, "circuitpython: MCP2221 setup failed\n");
        hid_host_close();
        return false;
    }
    _open = true;
    return true;
}

static void _backend_close(void) {
    if (!_open) {
        return;
    }
    if (!_lost) {
        _dir = 0;
        memset(_desig, MCP2221_GPIO, sizeof(_desig));
        _write_sram_gp();
    }
    hid_host_close();
    _open = false;
}

static void _backend_reset(uint32_t keep_mask) {
    uint8_t keep = (uint8_t)(keep_mask >> MCP2221_PIN_GP0) & 0x0F;
    _dir &= keep;
    _val &= keep;
    for (uint8_t gp = 0; gp < 4; gp++) {
        if (!((keep >> gp) & 1)) {
            _desig[gp] = MCP2221_GPIO;
        }
    }
    _write_sram_gp();
}

static digitalinout_result_t _backend_configure(uint8_t pin, bool output, bool open_drain, digitalio_pull_t pull) {
    if (pull != PULL_NONE) {
        return DIGITALINOUT_INVALID_PULL;
    }
    if (open_drain) {
        return DIGITALINOUT_INVALID_DRIVE_MODE;
    }
    if (!_pin_ok(pin)) {
        return DIGITALINOUT_OK;         // SCL and SDA belong to the I2C engine
    }
    uint8_t gp = pin - MCP2221_PIN_GP0;
    if (output) {
        _dir |= 1u << gp;
    } else {
        _dir &= ~(1u << gp);
    }
    if (_desig[gp] != MCP2221_GPIO) {
        _desig[gp] = MCP2221_GPIO;
        _write_sram_gp();
    } else {
        _write_gpio(gp);
    }
    return DIGITALINOUT_OK;
}

static bool _backend_write(uint8_t pin, bool value) {
    if (!_pin_ok(pin)) {
        return false;
    }
    uint8_t gp = pin - MCP2221_PIN_GP0;
    if (value) {
        _val |= 1u << gp;
    } else {
        _val &= ~(1u << gp);
    }
    return _write_gpio(gp);
}

static bool _backend_read(uint8_t pin, bool *level) {
    if (!_pin_ok(pin)) {
        return false;
    }
    uint8_t gp = pin - MCP2221_PIN_GP0;
    uint8_t cmd[] = { CMD_GET_GPIO };
    uint8_t reply[HID_REPORT_SIZE];
    if (!_xfer(cmd, sizeof(cmd), reply)) {
        return false;
    }
    uint8_t value = reply[2 + 2 * gp];
    *level = value != NOT_GPIO && value != 0;
    return true;
}

bool mcp2221_set_designation(uint8_t gp, uint8_t designation) {
    _desig[gp & 3] = designation;
    return _write_sram_gp();
}

bool mcp2221_adc_read(uint8_t channel, uint16_t *value) {
    uint8_t cmd[] = { CMD_STATUS };
    uint8_t reply[HID_REPORT_SIZE];
    if (channel < 1 || channel > 3 || !_xfer(cmd, sizeof(cmd), reply)) {
        return false;
    }
    *value = (uint16_t)(reply[50 + 2 * (channel - 1)] | (reply[51 + 2 * (channel - 1)] << 8));
    return true;
}

bool mcp2221_dac_set(uint8_t value) {
    _dac = value & 0x1F;
    uint8_t cmd[] = { CMD_SET_SRAM, 0, 0, 0, (uint8_t)(0x80 | _dac) };
    uint8_t reply[HID_REPORT_SIZE];
    return _xfer(cmd, sizeof(cmd), reply);
}

// ---- I2C through the chip's engine ----

static bool _i2c_cancel(void) {
    uint8_t cmd[] = { CMD_STATUS, 0, 0x10 };
    uint8_t reply[HID_REPORT_SIZE];
    return _xfer(cmd, sizeof(cmd), reply);
}

static bool _i2c_state(uint8_t *state) {
    uint8_t cmd[] = { CMD_STATUS };
    uint8_t reply[HID_REPORT_SIZE];
    if (!_xfer(cmd, sizeof(cmd), reply)) {
        return false;
    }
    *state = reply[8];
    return true;
}

static unsigned _i2c_timeout_ms(size_t len) {
    return 1000 + (unsigned)((uint64_t)len * 10 * 1000 / (_i2c_hz != 0 ? _i2c_hz : 100000));
}

static bool _i2c_construct(uint32_t frequency, uint32_t *actual_hz) {
    if (frequency < 47000) {
        frequency = 47000;
    }
    if (frequency > 400000) {
        frequency = 400000;
    }
    uint32_t divider = 12000000 / frequency - 3;
    uint8_t cmd[] = { CMD_STATUS, 0, 0, 0x20, (uint8_t)divider };
    uint8_t reply[HID_REPORT_SIZE];
    // 0x21: the engine is busy; an idle engine (cancelled, or the chip reset) takes the speed.
    if (!_engine_idle() || !_xfer(cmd, sizeof(cmd), reply) || reply[3] != 0x20) {
        return false;
    }
    _i2c_hz = 12000000 / (divider + 3);
    *actual_hz = _i2c_hz;
    _i2c_no_stop = false;
    return true;
}

static void _i2c_deinit(void) {
    _i2c_cancel();
    _i2c_no_stop = false;
}

// Wait for the engine to finish; -MP_ENODEV on an address NACK, -MP_EIO on anything else.
// After a write without stop the engine does not go idle but waits for the repeated start.
static int _i2c_wait_idle(unsigned timeout_ms, bool stop) {
    uint64_t deadline = desktop_host_ms() + timeout_ms;
    for (;;) {
        uint8_t state = 0;
        if (!_i2c_state(&state)) {
            return -MP_EIO;
        }
        if (state == I2C_STATE_IDLE || (!stop && state == I2C_STATE_WAIT_RESTART)) {
            return 0;
        }
        if (state == I2C_STATE_ADDRESS_NACK) {
            _i2c_cancel();
            return -MP_ENODEV;
        }
        if (desktop_host_ms() >= deadline) {
            _i2c_cancel();
            return -MP_EIO;
        }
        desktop_host_sleep_ms(1);
    }
}

static int _i2c_write(uint16_t address, const uint8_t *data, size_t len, bool stop) {
    if (_lost) {
        return -MP_EIO;
    }
    uint8_t opcode = stop ? CMD_I2C_WRITE : CMD_I2C_WRITE_NO_STOP;
    unsigned timeout = _i2c_timeout_ms(len);
    size_t done = 0;
    do {
        size_t take = len - done < I2C_CHUNK ? len - done : I2C_CHUNK;
        uint8_t cmd[4 + I2C_CHUNK] = { opcode, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8), (uint8_t)(address << 1) };
        memcpy(cmd + 4, data + done, take);
        uint8_t reply[HID_REPORT_SIZE];
        uint64_t deadline = desktop_host_ms() + timeout;
        for (;;) {
            if (!_xfer(cmd, 4 + take, reply)) {
                return -MP_EIO;
            }
            if (reply[1] == 0) {
                break;
            }
            // Busy: the engine still clocks the previous piece out, or a device NACKed it.
            if (reply[2] == I2C_STATE_ADDRESS_NACK) {
                _i2c_cancel();
                return done == 0 ? -MP_ENODEV : -MP_EIO;
            }
            if (desktop_host_ms() >= deadline) {
                _i2c_cancel();
                return -MP_EIO;
            }
            desktop_host_sleep_ms(1);
        }
        done += take;
    } while (done < len);
    int status = _i2c_wait_idle(timeout, stop);
    _i2c_no_stop = !stop && status == 0;
    return status;
}

static int _i2c_read(uint16_t address, uint8_t *data, size_t len) {
    if (_lost) {
        return -MP_EIO;
    }
    uint8_t cmd[] = { _i2c_no_stop ? CMD_I2C_READ_REPEATED : CMD_I2C_READ, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8), (uint8_t)((address << 1) | 1) };
    _i2c_no_stop = false;
    uint8_t reply[HID_REPORT_SIZE];
    unsigned timeout = _i2c_timeout_ms(len);
    uint64_t deadline = desktop_host_ms() + timeout;
    for (;;) {
        if (!_xfer(cmd, sizeof(cmd), reply)) {
            return -MP_EIO;
        }
        if (reply[1] == 0) {
            break;
        }
        if (desktop_host_ms() >= deadline) {
            _i2c_cancel();
            return -MP_EIO;
        }
        desktop_host_sleep_ms(1);
    }
    size_t got = 0;
    do {
        uint8_t fetch[] = { CMD_READ_DATA };
        if (!_xfer(fetch, sizeof(fetch), reply)) {
            return -MP_EIO;
        }
        // Byte 1: 0 = data follows, 0x41 = not yet (count 127); state 0x25 = no acknowledge.
        if (reply[2] == I2C_STATE_ADDRESS_NACK) {
            _i2c_cancel();
            return got == 0 ? -MP_ENODEV : -MP_EIO;
        }
        if (reply[1] != 0 || reply[3] == 0 || reply[3] > I2C_CHUNK) {
            if (reply[1] != 0 && reply[1] != 0x41) {
                _i2c_cancel();
                return -MP_EIO;
            }
            if (desktop_host_ms() >= deadline) {
                _i2c_cancel();
                return -MP_EIO;
            }
            desktop_host_sleep_ms(1);
            continue;
        }
        size_t count = reply[3];
        if (count > len - got) {
            count = len - got;
        }
        memcpy(data + got, reply + 4, count);
        got += count;
    } while (got < len);
    return 0;
}

static const desktop_i2c_ops_t _i2c_ops = {
    .scl = MCP2221_PIN_SCL,
    .sda = MCP2221_PIN_SDA,
    .construct = _i2c_construct,
    .deinit = _i2c_deinit,
    .write = _i2c_write,
    .read = _i2c_read,
};

const desktop_pin_backend_t desktop_mcp2221_backend = {
    .name = "MCP2221",
    .present = _backend_present,
    .open = _backend_open,
    .close = _backend_close,
    .reset = _backend_reset,
    .configure = _backend_configure,
    .write = _backend_write,
    .read = _backend_read,
    .lost = mcp2221_lost,
    .pin_ok = _pin_ok,
    #if CIRCUITPY_BUSIO_I2C
    .i2c = &_i2c_ops,
    #else
    .i2c = NULL,
    #endif
    .spi = NULL,
    .accepts_drive = false,
};
