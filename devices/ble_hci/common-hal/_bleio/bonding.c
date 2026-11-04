// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "bonding.h"

#include <string.h>

#include "py/mpconfig.h"

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

static uint32_t _crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xffffffff;
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
    return _crc32((const uint8_t *)store + from, sizeof(*store) - from);
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

bonding_record_t *bonding_find_peer(const bt_addr_le_t *own, const bt_addr_le_t *peer) {
    bonding_store_t *store = bonding_store();
    for (size_t i = 0; i < BONDING_MAX; i++) {
        if (_ours(&store->records[i], own) && _same_address(&store->records[i].peer, peer)) {
            return &store->records[i];
        }
    }
    return NULL;
}
