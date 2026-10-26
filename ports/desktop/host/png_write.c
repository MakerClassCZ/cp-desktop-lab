// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "png_write.h"

#include <stdlib.h>
#include <string.h>

#define STORED_MAX 65535            // the most a stored deflate block holds

static uint32_t _crc_table[256];

static uint32_t _crc(uint32_t crc, const uint8_t *data, size_t len) {
    if (_crc_table[1] == 0) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            _crc_table[n] = c;
        }
    }
    for (size_t i = 0; i < len; i++) {
        crc = _crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return crc;
}

static uint8_t *_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return p + 4;
}

// A chunk: length, type, data, and the CRC of type and data.
static uint8_t *_chunk(uint8_t *p, const char *type, const uint8_t *data, size_t len) {
    p = _be32(p, (uint32_t)len);
    uint8_t *typed = p;
    memcpy(p, type, 4);
    p += 4;
    if (len > 0 && data != p) {
        memmove(p, data, len);
    }
    p += len;
    return _be32(p, _crc(0xffffffffu, typed, len + 4) ^ 0xffffffffu);
}

size_t png_write_rgb565(const uint16_t *pixels, uint16_t width, uint16_t height, uint8_t **out) {
    // The raw image: a filter byte (0, none) before each row of RGB.
    size_t row = 1 + (size_t)width * 3, raw_len = row * height;
    size_t blocks = raw_len / STORED_MAX + 1;
    size_t idat_len = 2 + raw_len + blocks * 5 + 4;
    size_t total = 8 + (12 + 13) + (12 + idat_len) + 12;
    uint8_t *png = malloc(total);
    uint8_t *raw = malloc(raw_len);
    if (png == NULL || raw == NULL) {
        free(png);
        free(raw);
        return 0;
    }
    for (uint16_t y = 0; y < height; y++) {
        uint8_t *r = raw + row * y;
        *r++ = 0;
        for (uint16_t x = 0; x < width; x++) {
            uint16_t c = pixels[(size_t)y * width + x];
            uint8_t r5 = c >> 11, g6 = (c >> 5) & 0x3f, b5 = c & 0x1f;
            *r++ = (uint8_t)((r5 << 3) | (r5 >> 2));
            *r++ = (uint8_t)((g6 << 2) | (g6 >> 4));
            *r++ = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
    }
    uint8_t *p = png;
    memcpy(p, "\x89PNG\r\n\x1a\n", 8);
    p += 8;
    uint8_t ihdr[13];
    _be32(ihdr, width);
    _be32(ihdr + 4, height);
    ihdr[8] = 8;                    // bits per channel
    ihdr[9] = 2;                    // RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    p = _chunk(p, "IHDR", ihdr, sizeof(ihdr));

    // IDAT's data is written in place after its length and type, then the chunk is closed.
    uint8_t *idat = p + 8, *z = idat;
    *z++ = 0x78;                    // zlib: deflate, 32K window
    *z++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t done = 0; done < raw_len || done == 0;) {
        size_t n = raw_len - done < STORED_MAX ? raw_len - done : STORED_MAX;
        *z++ = done + n == raw_len ? 1 : 0;
        *z++ = (uint8_t)n;
        *z++ = (uint8_t)(n >> 8);
        *z++ = (uint8_t)~n;
        *z++ = (uint8_t)(~n >> 8);
        memcpy(z, raw + done, n);
        for (size_t i = 0; i < n; i++) {
            a = (a + raw[done + i]) % 65521;
            b = (b + a) % 65521;
        }
        z += n;
        done += n;
        if (n == 0) {
            break;
        }
    }
    z = _be32(z, (b << 16) | a);
    p = _chunk(p, "IDAT", idat, (size_t)(z - idat));
    p = _chunk(p, "IEND", NULL, 0);
    free(raw);
    *out = png;
    return (size_t)(p - png);
}
