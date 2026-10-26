// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// SPI through the FT232H's MPSSE engine: SCK = D0, MOSI = D1, MISO = D2. Modes 0 and 2: the
// engine clocks out on one edge and in on the other, as these modes want.

#include "py/mpconfig.h"

#if CIRCUITPY_BUSIO_SPI

#include <string.h>

#include "mpsse.h"

// One MPSSE data command moves at most CHUNK bytes. Full duplex goes in pieces the chip can hold
// in its FIFO towards the host: sending more before reading would stall the engine.
#define CHUNK 65536
#define PIECE 512

static bool _has_mosi;
static uint8_t _polarity;
static uint32_t _hz = 250000;

static bool _construct(bool has_mosi, bool has_miso) {
    _has_mosi = has_mosi;
    _polarity = 0;
    // Claiming does not configure: the engine drives SK and DO only as outputs.
    mpsse_gpio_set(0, true, false, false);
    if (has_mosi) {
        mpsse_gpio_set(1, true, false, false);
    }
    if (has_miso) {
        mpsse_gpio_set(2, false, false, false);
    }
    mpsse_set_clock(250000, false);
    _hz = mpsse_clock_hz();
    return !mpsse_lost();
}

static void _deinit(void) {
}

static desktop_spi_result_t _configure(uint32_t hz, uint8_t polarity, uint8_t phase, uint8_t bits, uint32_t *actual_hz) {
    if (bits != 8) {
        return DESKTOP_SPI_BAD_BITS;
    }
    if (phase != 0) {
        return DESKTOP_SPI_BAD_MODE;
    }
    mpsse_set_clock(hz, false);
    _hz = mpsse_clock_hz();
    _polarity = polarity;
    // The clock idles at the polarity level.
    mpsse_gpio_set(0, true, polarity != 0, false);
    *actual_hz = _hz;
    return DESKTOP_SPI_OK;
}

// The MPSSE data commands by clock polarity: bytes out on the edge the receiver does not sample.
static uint8_t _opcode(bool out, bool in) {
    if (_polarity == 0) {
        return out && in ? 0x31 : out ? 0x11 : 0x20;
    }
    return out && in ? 0x34 : out ? 0x10 : 0x24;
}

static unsigned _timeout_ms(size_t len) {
    return 1000 + (unsigned)((uint64_t)len * 8 * 1000 / (_hz != 0 ? _hz : 1));
}

static bool _transfer(const uint8_t *data_out, uint8_t fill, uint8_t *data_in, size_t len) {
    if (mpsse_lost()) {
        return false;
    }
    // Bytes go out whenever MOSI is wired, so a read clocks write_value out; without MOSI only in.
    bool out = data_out != NULL || _has_mosi;
    bool in = data_in != NULL;
    size_t step = out && in ? PIECE : CHUNK;
    uint8_t piece[PIECE];
    memset(piece, fill, sizeof(piece));
    while (len > 0) {
        size_t n = len < step ? len : step;
        uint8_t header[] = { _opcode(out, in), (uint8_t)((n - 1) & 0xFF), (uint8_t)((n - 1) >> 8) };
        if (!mpsse_send(header, sizeof(header))) {
            break;
        }
        if (out) {
            if (data_out != NULL) {
                if (!mpsse_send(data_out, n)) {
                    break;
                }
                data_out += n;
            } else {
                size_t left = n;
                while (left > 0) {
                    size_t m = left < sizeof(piece) ? left : sizeof(piece);
                    if (!mpsse_send(piece, m)) {
                        break;
                    }
                    left -= m;
                }
                if (left > 0) {
                    break;
                }
            }
        }
        if (in) {
            static const uint8_t flush = 0x87;
            if (!mpsse_send(&flush, 1) || !mpsse_recv(data_in, n, _timeout_ms(n))) {
                break;
            }
            data_in += n;
        }
        len -= n;
    }
    return len == 0;
}

const desktop_spi_ops_t mpsse_spi_ops = {
    .sck = 0,
    .mosi = 1,
    .miso = 2,
    .construct = _construct,
    .deinit = _deinit,
    .configure = _configure,
    .transfer = _transfer,
};

#endif
