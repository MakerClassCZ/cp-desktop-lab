// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

// The bond store: one blob in RAM, a header and BONDING_MAX records, kept where it survives a
// reset by the port (bleio_hci_bonding_storage_read/write), in RAM only by default. Records of
// several controller identities live side by side (each names ours), so a port that has more than
// one controller (a virtual one and a real one) keeps one store.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hci_include/addr.h"

#define BONDING_VERSION 1
#define BONDING_MAX 8
#define BONDING_CCCD_MAX 16

// A record's flags.
#define BONDING_IN_USE        0x01
#define BONDING_PEER_IRK      0x02  // peer_irk holds the peer's identity resolving key
#define BONDING_AUTHENTICATED 0x04  // MITM-protected keys
#define BONDING_SC            0x08  // LE Secure Connections keys

// A record's role: ours on the bond's links, as HCI numbers it (LE Connection Complete).
#define BONDING_ROLE_CENTRAL    0x00
#define BONDING_ROLE_PERIPHERAL 0x01

// The header's flags.
#define BONDING_HAVE_IRK            0x01
#define BONDING_HAVE_STATIC_ADDRESS 0x02

typedef struct __attribute__((packed)) {
    uint16_t handle;
    uint16_t value;
} bonding_cccd_t;

typedef struct __attribute__((packed)) {
    uint8_t flags;
    uint8_t role;
    bt_addr_le_t own;           // our identity address on this bond
    bt_addr_le_t peer;          // the peer's identity address
    uint8_t peer_irk[16];
    uint8_t ltk[16];
    uint16_t ediv;
    uint8_t rand[8];
    uint8_t key_size;
    uint32_t use;               // larger: used more recently
    uint32_t gatt_hash;         // of our GATT table when the CCCDs were taken
    uint8_t cccd_count;
    bonding_cccd_t cccds[BONDING_CCCD_MAX];
} bonding_record_t;

typedef struct __attribute__((packed)) {
    char magic[4];              // "CPBD"
    uint8_t version;            // BONDING_VERSION
    uint8_t flags;              // BONDING_HAVE_*
    uint16_t reserved;
    uint32_t crc;               // CRC-32 of every byte after this field
    uint8_t irk[16];            // ours, given to every bonded peer
    bt_addr_t static_address;   // the identity of a controller with neither public nor static address
    uint16_t reserved2;
} bonding_header_t;

typedef struct __attribute__((packed)) {
    bonding_header_t header;
    bonding_record_t records[BONDING_MAX];
} bonding_store_t;

// The port's hook: the blob where it survives a reset. The defaults (weak) keep nothing: the
// store is then in RAM only, for as long as the program runs.
// Bytes read into buf, 0 for none.
size_t bleio_hci_bonding_storage_read(uint8_t *buf, size_t len);
// False when the blob could not be kept.
bool bleio_hci_bonding_storage_write(const uint8_t *buf, size_t len);

// The store, read from the port's storage the first time it is asked for; a missing blob, or one
// of another magic, version or CRC, is an empty store. Neither needs the VM heap nor raises.
bonding_store_t *bonding_store(void);
// The store to the port's storage; false when that failed.
bool bonding_save(void);

// Whether a bond of our identity own (NULL: any identity) in role exists.
bool bonding_is_bonded(const bt_addr_le_t *own, uint8_t role);
// The bonds of our identity own (NULL: every bond) erased and the store saved when there were
// any; our IRK and static address stay. Their number.
size_t bonding_erase(const bt_addr_le_t *own);
// The bond of our identity own with the peer of address peer: its identity address, or a
// resolvable private address of its IRK. NULL when there is none.
bonding_record_t *bonding_find_peer(const bt_addr_le_t *own, const bt_addr_le_t *peer);
// The bond of our identity own, in role, whose LTK has this EDIV and Rand; NULL when none has.
bonding_record_t *bonding_find_ediv(const bt_addr_le_t *own, uint8_t role, uint16_t ediv,
    const uint8_t rand[8]);
// A new bond, in place of the same peer's (or of replaces, when not NULL), else in a free record,
// else in place of the least recently used one; marked as used now. Its index. Not saved.
size_t bonding_add(const bonding_record_t *record, const bonding_record_t *replaces);
// A bond marked as used now (the least recently used one goes first). Not saved.
void bonding_touch(bonding_record_t *record);
// The number of bonds of our identity own (NULL: every bond).
size_t bonding_count(const bt_addr_le_t *own);
// CRC-32 (as zlib's) of data, on from crc (0 to start).
uint32_t bonding_crc32(uint32_t crc, const uint8_t *data, size_t len);
