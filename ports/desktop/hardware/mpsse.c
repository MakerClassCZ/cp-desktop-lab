// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "mpsse.h"

#include <stdio.h>
#include <string.h>

#include "py/runtime.h"
#include "shared-bindings/digitalio/Pull.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "usb_host.h"
#include "desktop_port.h"

#define FTDI_VID 0x0403
#define FT232H_PID 0x6014
#define EP_IN 0x81
#define EP_OUT 0x02
#define PACKET_MAX 512      // wMaxPacketSize at high speed; 64 at full speed (a hub, a VM)

#define SIO_RESET 0x00
#define SIO_SET_EVENT_CHAR 0x06
#define SIO_SET_ERROR_CHAR 0x07
#define SIO_SET_LATENCY_TIMER 0x09
#define SIO_SET_BITMODE 0x0B
#define BITMODE_RESET 0x0000
#define BITMODE_MPSSE 0x0200

#define CLOCK_HZ 60000000u

static bool _open;
static bool _lost;
static uint64_t _busy_until;    // when the bytes sent so far will have been clocked out
static uint32_t _clock_hz;
static uint8_t _dir[2], _val[2], _zero[2];

// The chip prefixes every packet with two status bytes; the packet size comes from the descriptor.
static size_t _packet = PACKET_MAX;

// Payload that arrived beyond what the last recv asked for; one raw read's worth at most.
static uint8_t _pending[8 * PACKET_MAX];
static size_t _pending_len;

// A transfer gets a second plus the time its bytes take at the bus clock.
static unsigned _timeout_ms(size_t len) {
    uint32_t hz = _clock_hz != 0 ? _clock_hz : 1000000;
    return 1000 + (unsigned)((uint64_t)len * 8 * 1000 / hz);
}

bool mpsse_lost(void) {
    return _lost;
}

// The first USB error says why on stderr; Python only sees "lost".
static void _mark_lost(const char *what, int status) {
    if (!_lost) {
        fprintf(stderr, "circuitpython: FT232H lost: %s: %s\n", what, usb_host_strerror(status));
    }
    _lost = true;
}

static bool _control(uint8_t request, uint16_t value) {
    int status = usb_host_control(request, value, 1);
    if (status < 0) {
        _mark_lost("control request", status);
        return false;
    }
    return true;
}

bool mpsse_send(const uint8_t *cmd, size_t len) {
    if (_lost) {
        return false;
    }
    // The chip takes bytes only as its engine clocks the earlier ones out, so the timeout covers
    // what is still queued from before (at the clock of that time) and this stream at the clock
    // of now.
    uint64_t now = desktop_host_ms();
    unsigned timeout = _timeout_ms(len) + (unsigned)(_busy_until > now ? _busy_until - now : 0);
    size_t queued = len < 1024 ? len : 1024;
    while (len > 0) {
        int moved = usb_host_bulk_out(EP_OUT, cmd, len, timeout);
        if (moved <= 0) {
            _mark_lost("bulk out", moved);
            return false;
        }
        cmd += moved;
        len -= (size_t)moved;
    }
    // Once the transfer is in, the chip holds at most its 1 KB FIFO of it, at the clock of now;
    // what an earlier, slower stream left queued keeps draining at its own pace.
    uint64_t done = desktop_host_ms() + (_timeout_ms(queued) - 1000);
    if (done > _busy_until) {
        _busy_until = done;
    }
    return true;
}

static void _drain(void);

bool mpsse_recv(uint8_t *buf, size_t len, unsigned timeout_ms) {
    if (_lost) {
        return false;
    }
    size_t got = 0;
    if (_pending_len > 0) {
        size_t take = _pending_len < len ? _pending_len : len;
        memcpy(buf, _pending, take);
        memmove(_pending, _pending + take, _pending_len - take);
        _pending_len -= take;
        got = take;
    }
    uint64_t deadline = desktop_host_ms() + timeout_ms;
    uint8_t raw[8 * PACKET_MAX];
    while (got < len) {
        uint64_t now = desktop_host_ms();
        int moved = now < deadline ? usb_host_bulk_in(EP_IN, raw, sizeof(raw), (unsigned)(deadline - now)) : USB_HOST_ERROR_TIMEOUT;
        if (moved < 0) {
            if (moved != USB_HOST_ERROR_TIMEOUT) {
                _mark_lost("bulk in", moved);
            } else {
                // Late bytes must not become the next call's data.
                _drain();
            }
            return false;
        }
        for (int at = 0; at < moved; at += (int)_packet) {
            int end = at + (int)_packet < moved ? at + (int)_packet : moved;
            for (int i = at + 2; i < end; i++) {
                if (got < len) {
                    buf[got++] = raw[i];
                } else if (_pending_len < sizeof(_pending)) {
                    _pending[_pending_len++] = raw[i];
                }
            }
        }
    }
    return true;
}

// Read until the chip has nothing left: after a crash the engine may still be streaming.
static void _drain(void) {
    uint8_t raw[8 * PACKET_MAX];
    for (int i = 0; i < 400; i++) {
        if (usb_host_bulk_in(EP_IN, raw, sizeof(raw), 20) <= 2) {
            break;
        }
    }
    _pending_len = 0;
}

void mpsse_set_clock(uint32_t hz, bool three_phase) {
    uint32_t base = CLOCK_HZ / (three_phase ? 3 : 2);
    if (hz == 0) {
        hz = 1;
    }
    uint32_t div = base / hz;
    if (div > 0 && base / div > hz) {
        div++;
    }
    if (div == 0) {
        div = 1;
    }
    if (div > 0x10000) {
        div = 0x10000;
    }
    _clock_hz = base / div;
    uint8_t cmd[] = { three_phase ? 0x8C : 0x8D, 0x86, (uint8_t)((div - 1) & 0xFF), (uint8_t)(((div - 1) >> 8) & 0xFF) };
    mpsse_send(cmd, sizeof(cmd));
}

uint32_t mpsse_clock_hz(void) {
    return _clock_hz;
}

static bool _write_bank(uint8_t bank) {
    uint8_t cmd[] = { bank == 0 ? 0x80 : 0x82, _val[bank], _dir[bank] };
    return mpsse_send(cmd, sizeof(cmd));
}

static bool _write_zero(void) {
    uint8_t cmd[] = { 0x9E, _zero[0], _zero[1] };
    return mpsse_send(cmd, sizeof(cmd));
}

bool mpsse_gpio_set(uint8_t pin, bool output, bool value, bool drive_zero) {
    if (_lost) {
        return false;
    }
    uint8_t bank = pin >> 3;
    uint8_t bit = 1u << (pin & 7);
    uint8_t dir = output ? (_dir[bank] | bit) : (_dir[bank] & ~bit);
    uint8_t val = value ? (_val[bank] | bit) : (_val[bank] & ~bit);
    uint8_t zero = drive_zero ? (_zero[bank] | bit) : (_zero[bank] & ~bit);
    bool bank_changed = dir != _dir[bank] || val != _val[bank];
    bool zero_changed = zero != _zero[bank];
    // Drive-only-zero goes on before a line becomes an output, and off after it stops being one.
    if (zero_changed && drive_zero) {
        _zero[bank] = zero;
        if (!_write_zero()) {
            return false;
        }
    }
    if (bank_changed) {
        _dir[bank] = dir;
        _val[bank] = val;
        if (!_write_bank(bank)) {
            return false;
        }
    }
    if (zero_changed && !drive_zero) {
        _zero[bank] = zero;
        if (!_write_zero()) {
            return false;
        }
    }
    return true;
}

uint8_t mpsse_bank_value(uint8_t bank) {
    return _val[bank & 1];
}

uint8_t mpsse_bank_dir(uint8_t bank) {
    return _dir[bank & 1];
}

bool mpsse_gpio_get(uint8_t pin, bool *level) {
    if (_lost) {
        return false;
    }
    uint8_t bank = pin >> 3;
    uint8_t cmd[] = { bank == 0 ? 0x81 : 0x83, 0x87 };
    uint8_t level_byte = 0;
    if (!mpsse_send(cmd, sizeof(cmd)) || !mpsse_recv(&level_byte, 1, _timeout_ms(1))) {
        return false;
    }
    *level = (level_byte >> (pin & 7)) & 1;
    return true;
}

void mpsse_gpio_reset(uint16_t keep_mask) {
    if (_lost) {
        return;
    }
    uint8_t keep[2] = { (uint8_t)(keep_mask & 0xFF), (uint8_t)(keep_mask >> 8) };
    for (uint8_t bank = 0; bank < 2; bank++) {
        _dir[bank] &= keep[bank];
        _val[bank] &= keep[bank];
        _zero[bank] &= keep[bank];
        _write_bank(bank);
    }
    _write_zero();
}

bool mpsse_open(void) {
    if (_open) {
        return true;
    }
    char err[256];
    if (!usb_host_load(err, sizeof(err)) || !usb_host_open(FTDI_VID, FT232H_PID, err, sizeof(err))) {
        fprintf(stderr, "circuitpython: %s\n", err);
        #ifdef _WIN32
        fprintf(stderr, "  switch the driver to WinUSB with Zadig (Options > List All Devices > \"USB Serial Converter\" 0403 6014)\n");
        #elif !defined(__APPLE__)
        fprintf(stderr, "  udev rule: SUBSYSTEM==\"usb\", ATTR{idVendor}==\"0403\", ATTR{idProduct}==\"6014\", MODE=\"0666\"\n");
        #endif
        return false;
    }
    _lost = false;
    _pending_len = 0;
    _clock_hz = 0;
    int packet = usb_host_max_packet(EP_IN);
    _packet = packet > 0 && packet <= PACKET_MAX ? (size_t)packet : PACKET_MAX;
    memset(_dir, 0, sizeof(_dir));
    memset(_val, 0, sizeof(_val));
    memset(_zero, 0, sizeof(_zero));
    bool ok = _control(SIO_RESET, 0)
        && _control(SIO_RESET, 1)       // purge RX
        && _control(SIO_RESET, 2)       // purge TX
        && _control(SIO_SET_LATENCY_TIMER, 255)
        && _control(SIO_SET_EVENT_CHAR, 0)
        && _control(SIO_SET_ERROR_CHAR, 0)
        && _control(SIO_SET_BITMODE, BITMODE_RESET)
        && _control(SIO_SET_BITMODE, BITMODE_MPSSE);
    if (!ok) {
        fprintf(stderr, "circuitpython: FT232H setup failed\n");
        usb_host_close();
        return false;
    }
    desktop_host_sleep_ms(50);
    _drain();
    // 60 MHz clock, no adaptive clocking, no loopback; a bad command answers 0xFA + the byte. The
    // latency timer is at its longest: the chip holds about two packets towards the host and
    // stalls the engine on a third, so replies must form full packets, and every command stream
    // ends with 0x87 (send immediate) instead of waiting for the timer.
    static const uint8_t setup[] = { 0x8A, 0x97, 0x85, 0xAA, 0x87 };
    uint8_t reply[2] = { 0, 0 };
    bool synced = false;
    for (int attempt = 0; attempt < 2 && !synced; attempt++) {
        synced = mpsse_send(setup, sizeof(setup)) && mpsse_recv(reply, 2, 500) && reply[0] == 0xFA && reply[1] == 0xAA;
        if (!synced && !_lost) {
            _drain();
        }
    }
    if (!synced) {
        fprintf(stderr, "circuitpython: the FT232H's MPSSE engine does not answer (%02x %02x)\n", reply[0], reply[1]);
        usb_host_close();
        return false;
    }
    mpsse_set_clock(1000000, false);
    mpsse_gpio_reset(0);
    _open = !_lost;
    return _open;
}

void mpsse_close(void) {
    if (!_open) {
        return;
    }
    if (!_lost) {
        mpsse_gpio_reset(0);
        _control(SIO_SET_BITMODE, BITMODE_RESET);
    }
    usb_host_close();
    _open = false;
}

// The pin backend: a value written is remembered per pin so a direction change keeps it.
static uint16_t _written;

static bool _present(void) {
    return usb_host_present(FTDI_VID, FT232H_PID);
}

static digitalinout_result_t _configure(uint8_t pin, bool output, bool open_drain, digitalio_pull_t pull) {
    if (pull == PULL_DOWN) {
        return DIGITALINOUT_INVALID_PULL;
    }
    mpsse_gpio_set(pin, output, (_written >> pin) & 1, open_drain);
    return DIGITALINOUT_OK;
}

static bool _write(uint8_t pin, bool value) {
    if (value) {
        _written |= 1u << pin;
    } else {
        _written &= ~(1u << pin);
    }
    uint8_t bank = pin >> 3;
    uint8_t bit = 1u << (pin & 7);
    // The direction stays: switch_to_output sets the level first and the direction (with
    // drive-only-zero in the right order) through configure.
    return mpsse_gpio_set(pin, (_dir[bank] & bit) != 0, value, (_zero[bank] & bit) != 0);
}

static void _reset(uint32_t keep_mask) {
    _written &= (uint16_t)keep_mask;
    mpsse_gpio_reset((uint16_t)keep_mask);
}

const desktop_pin_backend_t desktop_ft232h_backend = {
    .name = "FT232H",
    .present = _present,
    .open = mpsse_open,
    .close = mpsse_close,
    .reset = _reset,
    .configure = _configure,
    .write = _write,
    .read = mpsse_gpio_get,
    .lost = mpsse_lost,
    .pin_ok = NULL,
    #if CIRCUITPY_BUSIO_I2C
    .i2c = &mpsse_i2c_ops,
    #else
    .i2c = NULL,
    #endif
    #if CIRCUITPY_BUSIO_SPI
    .spi = &mpsse_spi_ops,
    #else
    .spi = NULL,
    #endif
    .accepts_drive = false,
};
