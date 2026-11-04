// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

// The Security Manager's functions for LE Legacy pairing and privacy (Core Vol 3 Part H 2.2),
// over AES-128 in software. Every value is least significant byte first, as SMP sends it and as
// HCI's LE Encrypt takes it.

#include <stdint.h>

// e(key, data): AES-128.
void smp_e(const uint8_t key[16], const uint8_t data[16], uint8_t out[16]);
// The confirm value of a random value r: preq and pres the 7-byte Pairing Request and Response,
// iat/ia the initiator's address type (0 public, 1 random) and address, rat/ra the responder's.
void smp_c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7], const uint8_t pres[7],
    uint8_t iat, const uint8_t ia[6], uint8_t rat, const uint8_t ra[6], uint8_t out[16]);
// The STK of the responder's r1 and the initiator's r2.
void smp_s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16], uint8_t out[16]);
// The hash of a resolvable private address's 3-byte prand under an IRK.
void smp_ah(const uint8_t irk[16], const uint8_t r[3], uint8_t out[3]);
