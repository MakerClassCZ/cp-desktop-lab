// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "bonding.h"

#include <string.h>

#include "py/mpconfig.h"

#include "smp_crypto.h"

static const char _magic[4] = { 'C', 'P', 'B', 'D' };

static bonding_store_t _store;
static bool _loaded;

MP_WEAK size_t bleio_hci_bonding_storage_read(uint8_t *buf, size_t len) {
    (void)buf;
    (void)len;
    return 0;
}

MP_WEAK bool bleio_hci_bonding_storage_write(const uint8_t *buf, size_t len) {
    (void)buf;
    (void)len;
    return true;
}

uint32_t bonding_crc32(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
        }
    }
    return ~crc;
}

// What the CRC covers: everything after the field.
static uint32_t _store_crc(const bonding_store_t *store) {
    const size_t from = offsetof(bonding_header_t, crc) + sizeof(store->header.crc);
    return bonding_crc32(0, (const uint8_t *)store + from, sizeof(*store) - from);
}

static void _empty(void) {
    memset(&_store, 0, sizeof(_store));
    memcpy(_store.header.magic, _magic, sizeof(_magic));
    _store.header.version = BONDING_VERSION;
}

bonding_store_t *bonding_store(void) {
    if (!_loaded) {
        _loaded = true;
        size_t n = bleio_hci_bonding_storage_read((uint8_t *)&_store, sizeof(_store));
        if (n != sizeof(_store) || memcmp(_store.header.magic, _magic, sizeof(_magic)) != 0 ||
            _store.header.version != BONDING_VERSION || _store.header.crc != _store_crc(&_store)) {
            _empty();
        }
    }
    return &_store;
}

bool bonding_save(void) {
    bonding_store_t *store = bonding_store();
    store->header.crc = _store_crc(store);
    return bleio_hci_bonding_storage_write((const uint8_t *)store, sizeof(*store));
}

static bool _same_address(const bt_addr_le_t *a, const bt_addr_le_t *b) {
    return a->type == b->type && memcmp(a->a.val, b->a.val, sizeof(a->a.val)) == 0;
}

// A record in use, of our identity own (NULL: any).
static bool _ours(const bonding_record_t *record, const bt_addr_le_t *own) {
    return (record->flags & BONDING_IN_USE) && (own == NULL || _same_address(&record->own, own));
}

bool bonding_is_bonded(const bt_addr_le_t *own, uint8_t role) {
    bonding_store_t *store = bonding_store();
    for (size_t i = 0; i < BONDING_MAX; i++) {
        if (_ours(&store->records[i], own) && store->records[i].role == role) {
            return true;
        }
    }
    return false;
}

size_t bonding_erase(const bt_addr_le_t *own) {
    bonding_store_t *store = bonding_store();
    size_t erased = 0;
    for (size_t i = 0; i < BONDING_MAX; i++) {
        if (_ours(&store->records[i], own)) {
            memset(&store->records[i], 0, sizeof(store->records[i]));
            erased++;
        }
    }
    if (erased > 0) {
        bonding_save();
    }
    return erased;
}

// Whether a resolvable private address (random, its two top bits 01) is one of irk's.
static bool _resolves(const bt_addr_le_t *address, const uint8_t irk[16]) {
    if (address->type != BT_ADDR_LE_RANDOM || (address->a.val[5] & 0xc0) != 0x40) {
        return false;
    }
    uint8_t hash[3];
    smp_ah(irk, address->a.val + 3, hash);
    return memcmp(hash, address->a.val, sizeof(hash)) == 0;
}

bonding_record_t *bonding_find_peer(const bt_addr_le_t *own, const bt_addr_le_t *peer) {
    bonding_store_t *store = bonding_store();
    for (size_t i = 0; i < BONDING_MAX; i++) {
        bonding_record_t *record = &store->records[i];
        if (_ours(record, own) && (_same_address(&record->peer, peer) ||
                                   ((record->flags & BONDING_PEER_IRK) && _resolves(peer, record->peer_irk)))) {
            return record;
        }
    }
    return NULL;
}

bonding_record_t *bonding_find_ediv(const bt_addr_le_t *own, uint8_t role, uint16_t ediv,
    const uint8_t rand[8]) {
    bonding_store_t *store = bonding_store();
    for (size_t i = 0; i < BONDING_MAX; i++) {
        bonding_record_t *record = &store->records[i];
        if (_ours(record, own) && record->role == role && record->ediv == ediv &&
            memcmp(record->rand, rand, sizeof(record->rand)) == 0) {
            return record;
        }
    }
    return NULL;
}

static uint32_t _next_use(void) {
    bonding_store_t *store = bonding_store();
    uint32_t use = 0;
    for (size_t i = 0; i < BONDING_MAX; i++) {
        if ((store->records[i].flags & BONDING_IN_USE) && store->records[i].use > use) {
            use = store->records[i].use;
        }
    }
    return use + 1;
}

void bonding_touch(bonding_record_t *record) {
    record->use = _next_use();
}

size_t bonding_add(const bonding_record_t *record, const bonding_record_t *replaces) {
    bonding_store_t *store = bonding_store();
    // The one given, else the same peer's bond (it paired again: it forgot us); else a free one;
    // else the least recently used. Any other bond of the same peer goes: one peer, one bond.
    size_t at = replaces != NULL ? (size_t)(replaces - store->records) : BONDING_MAX;
    for (size_t i = 0; i < BONDING_MAX; i++) {
        bonding_record_t *r = &store->records[i];
        if (i == at || !_ours(r, &record->own) || r->role != record->role ||
            !_same_address(&r->peer, &record->peer)) {
            continue;
        }
        if (at == BONDING_MAX) {
            at = i;
        } else {
            memset(r, 0, sizeof(*r));
        }
    }
    for (size_t i = 0; i < BONDING_MAX && at == BONDING_MAX; i++) {
        if (!(store->records[i].flags & BONDING_IN_USE)) {
            at = i;
        }
    }
    if (at == BONDING_MAX) {
        at = 0;
        for (size_t i = 1; i < BONDING_MAX; i++) {
            if (store->records[i].use < store->records[at].use) {
                at = i;
            }
        }
    }
    const uint32_t use = _next_use();
    store->records[at] = *record;
    store->records[at].use = use;
    return at;
}

size_t bonding_count(const bt_addr_le_t *own) {
    bonding_store_t *store = bonding_store();
    size_t n = 0;
    for (size_t i = 0; i < BONDING_MAX; i++) {
        n += _ours(&store->records[i], own);
    }
    return n;
}
