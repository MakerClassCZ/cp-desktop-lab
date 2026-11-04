// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "smp_crypto.h"

#include <string.h>

#include "shared-module/aesio/aes.h"

// AES takes its key and block most significant byte first: SMP's values are turned around on the
// way in and out, as Zephyr's bt_encrypt_le() does.
static void _reverse(uint8_t *out, const uint8_t *in, size_t len) {
    for (size_t i = 0; i < len; i++) {
        out[i] = in[len - 1 - i];
    }
}

void smp_e(const uint8_t key[16], const uint8_t data[16], uint8_t out[16]) {
    uint8_t k[16], block[16];
    _reverse(k, key, 16);
    _reverse(block, data, 16);
    struct AES_ctx ctx;
    AES_init_ctx(&ctx, k, AES_KEYLEN128);
    AES_ECB_encrypt(&ctx, block);
    _reverse(out, block, 16);
}

static void _xor(uint8_t out[16], const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 16; i++) {
        out[i] = a[i] ^ b[i];
    }
}

void smp_c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7], const uint8_t pres[7],
    uint8_t iat, const uint8_t ia[6], uint8_t rat, const uint8_t ra[6], uint8_t out[16]) {
    // p1 = pres || preq || rat || iat, p2 = padding || ia || ra (most significant first).
    uint8_t p1[16], p2[16] = { 0 }, t[16];
    p1[0] = iat;
    p1[1] = rat;
    memcpy(p1 + 2, preq, 7);
    memcpy(p1 + 9, pres, 7);
    memcpy(p2, ra, 6);
    memcpy(p2 + 6, ia, 6);
    // c1 = e(k, e(k, r XOR p1) XOR p2)
    _xor(t, r, p1);
    smp_e(k, t, t);
    _xor(t, t, p2);
    smp_e(k, t, out);
}

void smp_s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16], uint8_t out[16]) {
    // r' = r1' || r2': the least significant halves, r1's on top.
    uint8_t r[16];
    memcpy(r, r2, 8);
    memcpy(r + 8, r1, 8);
    smp_e(k, r, out);
}

void smp_ah(const uint8_t irk[16], const uint8_t r[3], uint8_t out[3]) {
    // ah = e(k, padding || r) mod 2^24
    uint8_t t[16] = { 0 };
    memcpy(t, r, 3);
    smp_e(irk, t, t);
    memcpy(out, t, 3);
}
