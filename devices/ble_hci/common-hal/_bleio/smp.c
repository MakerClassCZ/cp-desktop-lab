// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The Security Manager as the responder of LE Legacy pairing, "Just Works" (Core Vol 3 Part H 2.3
// and 3): no MITM protection, no LE Secure Connections. The peer (a phone) pairs; we answer, give
// it our LTK with its EDIV and Rand and our identity, take its identity, and keep the bond. On a
// later connection the peer encrypts with that LTK, found by its EDIV and Rand.
//
// The states are laid out so that Secure Connections can come in between: its Pairing Public Key
// and DHKey Check would be states of their own after the Pairing Response.

#include "smp.h"

#include <string.h>

#include "py/mphal.h"
#include "shared/runtime/interrupt_char.h"
#include "py/runtime.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Adapter.h"
#include "shared-bindings/_bleio/Characteristic.h"
#include "shared-bindings/_bleio/Descriptor.h"
#include "shared-bindings/_bleio/Service.h"
#include "shared-bindings/_bleio/UUID.h"
#include "supervisor/shared/tick.h"

#include "common-hal/_bleio/Adapter.h"
#include "common-hal/_bleio/Connection.h"
#include "bonding.h"
#include "hci.h"
#include "hci_include/l2cap_internal.h"
#include "smp_crypto.h"

// The PDUs (Core Vol 3 Part H 3.3).
#define SMP_PAIRING_REQUEST 0x01
#define SMP_PAIRING_RESPONSE 0x02
#define SMP_PAIRING_CONFIRM 0x03
#define SMP_PAIRING_RANDOM 0x04
#define SMP_PAIRING_FAILED 0x05
#define SMP_ENCRYPTION_INFORMATION 0x06
#define SMP_CENTRAL_IDENTIFICATION 0x07
#define SMP_IDENTITY_INFORMATION 0x08
#define SMP_IDENTITY_ADDRESS_INFORMATION 0x09
#define SMP_SIGNING_INFORMATION 0x0a
#define SMP_SECURITY_REQUEST 0x0b

// Pairing Failed's reasons.
#define SMP_ERR_CONFIRM_VALUE 0x04
#define SMP_ERR_PAIRING_NOT_SUPPORTED 0x05
#define SMP_ERR_ENC_KEY_SIZE 0x06
#define SMP_ERR_CMD_NOT_SUPPORTED 0x07
#define SMP_ERR_UNSPECIFIED 0x08
#define SMP_ERR_INVALID_PARAMS 0x0a

#define SMP_IO_NO_INPUT_NO_OUTPUT 0x03
#define SMP_AUTH_BONDING 0x01
#define SMP_DIST_ENC_KEY 0x01
#define SMP_DIST_ID_KEY 0x02

#define SMP_MIN_KEY_SIZE 7
#define SMP_MAX_KEY_SIZE 16
#define SMP_TIMEOUT_MS 30000
// A bonded peer's CCCD changes are saved together, this long after the last.
#define SMP_CCCD_SAVE_MS 500

enum {
    SMP_IDLE,
    SMP_WAIT_CONFIRM,       // our Pairing Response sent: the peer's confirm value next
    SMP_WAIT_RANDOM,        // our confirm value sent: the peer's random value next
    SMP_WAIT_ENCRYPT,       // our random value sent: the peer encrypts with the STK
    SMP_WAIT_KEYS,          // encrypted, our keys sent: the peer's next
};

// What a handler leaves for smp_background().
#define PENDING_CONNECTED 0x01
#define PENDING_LTK 0x02
#define PENDING_ENCRYPTION 0x04

static bool smp_background_running;
// The store changed where it could not be saved (a handler): smp_background() saves it.
static bool smp_save_pending;

static const bt_addr_le_t *_own_identity(void) {
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    return adapter->identity_known ? &adapter->identity : NULL;
}

static bleio_connection_internal_t *_connection(uint16_t conn_handle) {
    return bleio_conn_handle_to_connection(conn_handle);
}

static void _send(bleio_connection_internal_t *c, const uint8_t *pdu, size_t len) {
    hci_send_acl_pkt(c->conn_handle, BT_L2CAP_CID_SMP, len, (uint8_t *)pdu);
    c->smp.last_pdu_ms = supervisor_ticks_ms64();
}

// n random bytes from the controller (LE Rand).
static bool _random(uint8_t *out, size_t n) {
    while (n > 0) {
        uint8_t r[8];
        if (hci_le_rand(r) != HCI_OK) {
            return false;
        }
        size_t take = n < sizeof(r) ? n : sizeof(r);
        memcpy(out, r, take);
        out += take;
        n -= take;
    }
    return true;
}

static void _fail(bleio_connection_internal_t *c, uint8_t reason) {
    uint8_t pdu[2] = { SMP_PAIRING_FAILED, reason };
    _send(c, pdu, sizeof(pdu));
    c->smp.state = SMP_IDLE;
    c->smp.result = reason;
    c->smp.security_requested = false;
}

// ---- Bonds and the GATT table ----

// The table read directly, its static part (_bleio's and the BLE workflow's attributes) then the
// program's: bleio_adapter_get_attribute() would enable a port's adapter that is off, from a
// handler too. One past the last handle; the attribute at a handle below that.
static size_t _attribute_end(void) {
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    return adapter->static_attribute_count + (adapter->attributes != NULL ? adapter->attributes->len : 0);
}

static mp_obj_t _attribute_at(size_t handle) {
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    return handle < adapter->static_attribute_count ? adapter->static_attributes[handle]
           : adapter->attributes->items[handle - adapter->static_attribute_count];
}

// A CRC-32 of each attribute's handle, kind, UUID and properties: a bonded peer's cached table is
// still ours when it matches.
static uint32_t _gatt_hash(void) {
    uint32_t crc = 0;
    for (size_t handle = 1; handle < _attribute_end(); handle++) {
        mp_obj_t obj = _attribute_at(handle);
        uint8_t entry[2 + 1 + 16 + 1];
        size_t n = 3;
        bleio_uuid_obj_t *uuid = NULL;
        entry[0] = handle & 0xff;
        entry[1] = handle >> 8;
        entry[2] = 0;
        if (mp_obj_is_type(obj, &bleio_service_type)) {
            entry[2] = 1;
            uuid = ((bleio_service_obj_t *)MP_OBJ_TO_PTR(obj))->uuid;
        } else if (mp_obj_is_type(obj, &bleio_characteristic_type)) {
            bleio_characteristic_obj_t *ch = MP_OBJ_TO_PTR(obj);
            // The declaration and the value hold the same object.
            entry[2] = ch->handle == handle ? 3 : 2;
            uuid = ch->uuid;
            entry[n++] = ch->props;
        } else if (mp_obj_is_type(obj, &bleio_descriptor_type)) {
            entry[2] = 4;
            uuid = ((bleio_descriptor_obj_t *)MP_OBJ_TO_PTR(obj))->uuid;
        }
        if (uuid != NULL) {
            common_hal_bleio_uuid_pack_into(uuid, entry + n);
            n += common_hal_bleio_uuid_get_size(uuid) == 16 ? 2 : 16;
        }
        crc = bonding_crc32(crc, entry, n);
    }
    return crc;
}

static bleio_descriptor_obj_t *_cccd_at(uint16_t handle) {
    if (handle == 0 || handle >= _attribute_end()) {
        return NULL;
    }
    mp_obj_t obj = _attribute_at(handle);
    if (!mp_obj_is_type(obj, &bleio_descriptor_type)) {
        return NULL;
    }
    bleio_descriptor_obj_t *d = MP_OBJ_TO_PTR(obj);
    return bleio_uuid_get_uuid16_or_unknown(d->uuid) == BLE_UUID_CCCD ? d : NULL;
}

// The CCCDs that are on, with the table's hash, into a bond.
static void _snapshot_cccds(bonding_record_t *record) {
    record->cccd_count = 0;
    record->gatt_hash = _gatt_hash();
    for (size_t handle = 1; handle < _attribute_end() && record->cccd_count < BONDING_CCCD_MAX; handle++) {
        bleio_descriptor_obj_t *d = _cccd_at(handle);
        if (d == NULL) {
            continue;
        }
        uint16_t value = 0;
        common_hal_bleio_descriptor_get_value(d, (uint8_t *)&value, sizeof(value));
        if (value != 0) {
            record->cccds[record->cccd_count].handle = handle;
            record->cccds[record->cccd_count].value = value;
            record->cccd_count++;
        }
    }
}

static void _set_cccd(uint16_t handle, uint16_t value) {
    bleio_descriptor_obj_t *d = _cccd_at(handle);
    if (d != NULL) {
        mp_buffer_info_t buf = { .buf = &value, .len = sizeof(value) };
        common_hal_bleio_descriptor_set_value(d, &buf);
    }
}

// A bonded peer's encrypted reconnect: its CCCDs back, as it does not write them again. When the
// table changed since, only Service Changed's comes back, and is indicated for every handle, so
// the peer discovers anew rather than use handles that are gone.
static void _restore_cccds(bleio_connection_internal_t *c, bonding_record_t *record) {
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    if (_attribute_end() <= 1) {
        return;
    }
    const uint32_t hash = _gatt_hash();
    if (record->gatt_hash == hash) {
        for (uint8_t i = 0; i < record->cccd_count && i < BONDING_CCCD_MAX; i++) {
            _set_cccd(record->cccds[i].handle, record->cccds[i].value);
        }
        return;
    }
    bleio_characteristic_obj_t *sc = adapter->service_changed_characteristic;
    const uint16_t sc_cccd = sc != NULL && sc->cccd != NULL ? sc->cccd->handle : 0;
    uint16_t sc_value = 0;
    for (uint8_t i = 0; i < record->cccd_count && i < BONDING_CCCD_MAX; i++) {
        if (record->cccds[i].handle == sc_cccd) {
            sc_value = record->cccds[i].value;
        }
    }
    record->cccd_count = 0;
    record->gatt_hash = hash;
    if (sc_value != 0) {
        _set_cccd(sc_cccd, sc_value);
        record->cccds[0].handle = sc_cccd;
        record->cccds[0].value = sc_value;
        record->cccd_count = 1;
    }
    bonding_save();
    if (sc_value & 0x0002) {
        // Service Changed for 0x0001-0xFFFF, not waited for: its confirmation comes to att.c.
        const uint8_t pdu[7] = { BT_ATT_OP_INDICATE, sc->handle & 0xff, sc->handle >> 8, 0x01, 0x00, 0xff, 0xff };
        hci_send_acl_pkt(c->conn_handle, BT_L2CAP_CID_ATT, sizeof(pdu), (uint8_t *)pdu);
    }
}

static bonding_record_t *_link_bond(bleio_connection_internal_t *c) {
    if (c->smp.bond < 0) {
        return NULL;
    }
    // Still this peer's: the record may have been erased and taken by another bond since.
    bonding_record_t *record = &bonding_store()->records[c->smp.bond];
    // The link keeps the bond's LTK and EDIV (stk, ediv): they tell it from a bond that took its
    // record since (the record erased, or the least used one replaced).
    const bool same = (record->flags & BONDING_IN_USE) && record->ediv == c->smp.ediv &&
        memcmp(record->ltk, c->smp.stk, sizeof(record->ltk)) == 0;
    if (!same) {
        c->smp.bond = -1;
        return NULL;
    }
    return record;
}

// ---- Pairing ----

static void _pairing_request(bleio_connection_internal_t *c, const uint8_t *pdu, size_t len) {
    smp_conn_t *s = &c->smp;
    if (c->is_central) {
        // A peripheral does not ask a central to pair; the central's side is not built.
        _fail(c, SMP_ERR_CMD_NOT_SUPPORTED);
        return;
    }
    if (len != 7) {
        _fail(c, SMP_ERR_INVALID_PARAMS);
        return;
    }
    if (s->state != SMP_IDLE) {
        _fail(c, SMP_ERR_UNSPECIFIED);
        return;
    }
    // Here a port could refuse a pairing (the BLE workflow: only in discovery mode).
    const uint8_t auth = pdu[3], max_key_size = pdu[4];
    if (max_key_size < SMP_MIN_KEY_SIZE) {
        _fail(c, SMP_ERR_ENC_KEY_SIZE);
        return;
    }
    if (max_key_size > SMP_MAX_KEY_SIZE) {
        _fail(c, SMP_ERR_INVALID_PARAMS);
        return;
    }
    // A bond needs both sides' Bonding flag: the peer's, and no pair(bond=False) of ours. Keys are
    // distributed for a bond only: ours as far as the peer asks, its identity as far as it offers.
    s->bonding = (auth & 0x03) == SMP_AUTH_BONDING && !s->no_bond;
    s->send_keys = s->bonding ? pdu[6] & (SMP_DIST_ENC_KEY | SMP_DIST_ID_KEY) : 0;
    if (_own_identity() == NULL) {
        s->send_keys &= ~SMP_DIST_ID_KEY;
    }
    s->take_keys = s->bonding ? pdu[5] & SMP_DIST_ID_KEY : 0;
    s->key_size = max_key_size;
    s->result = SMP_RESULT_NONE;
    _link_bond(c);      // the link's bond, if still its own (else forgotten), for _complete()
    s->have_peer_irk = s->have_peer_identity = false;
    memcpy(s->preq, pdu, 7);
    s->prsp[0] = SMP_PAIRING_RESPONSE;
    s->prsp[1] = SMP_IO_NO_INPUT_NO_OUTPUT;
    s->prsp[2] = 0;                 // no OOB data
    s->prsp[3] = s->bonding ? SMP_AUTH_BONDING : 0;     // no MITM, no Secure Connections
    s->prsp[4] = SMP_MAX_KEY_SIZE;
    s->prsp[5] = s->take_keys;
    s->prsp[6] = s->send_keys;
    _send(c, s->prsp, sizeof(s->prsp));
    s->state = SMP_WAIT_CONFIRM;
}

// Our confirm value of r, or the one we expect of the peer's: c1 with a TK of 0 (Just Works), the
// peer the initiator, we the responder.
static void _confirm_of(bleio_connection_internal_t *c, const uint8_t r[16], uint8_t out[16]) {
    static const uint8_t tk[16] = { 0 };
    smp_c1(tk, r, c->smp.preq, c->smp.prsp, c->addr.type, c->addr.a.val,
        c->smp.own.type, c->smp.own.a.val, out);
}

static void _pairing_confirm(bleio_connection_internal_t *c, const uint8_t *pdu, size_t len) {
    smp_conn_t *s = &c->smp;
    if (s->state != SMP_WAIT_CONFIRM || len != 17) {
        _fail(c, SMP_ERR_UNSPECIFIED);
        return;
    }
    memcpy(s->confirm, pdu + 1, 16);
    if (!_random(s->random, sizeof(s->random))) {
        _fail(c, SMP_ERR_UNSPECIFIED);
        return;
    }
    uint8_t answer[17] = { SMP_PAIRING_CONFIRM };
    _confirm_of(c, s->random, answer + 1);
    _send(c, answer, sizeof(answer));
    s->state = SMP_WAIT_RANDOM;
}

static void _pairing_random(bleio_connection_internal_t *c, const uint8_t *pdu, size_t len) {
    smp_conn_t *s = &c->smp;
    if (s->state != SMP_WAIT_RANDOM || len != 17) {
        _fail(c, SMP_ERR_UNSPECIFIED);
        return;
    }
    uint8_t expected[16];
    _confirm_of(c, pdu + 1, expected);
    if (memcmp(expected, s->confirm, sizeof(expected)) != 0) {
        _fail(c, SMP_ERR_CONFIRM_VALUE);
        return;
    }
    uint8_t answer[17] = { SMP_PAIRING_RANDOM };
    memcpy(answer + 1, s->random, 16);
    _send(c, answer, sizeof(answer));
    // STK = s1(TK, Srand, Mrand), cut to the agreed key size.
    static const uint8_t tk[16] = { 0 };
    smp_s1(tk, s->random, pdu + 1, s->stk);
    memset(s->stk + s->key_size, 0, 16 - s->key_size);
    s->state = SMP_WAIT_ENCRYPT;
}

// The pairing done: a bond when both asked for one, with this link's CCCDs.
static void _complete(bleio_connection_internal_t *c) {
    smp_conn_t *s = &c->smp;
    const bt_addr_le_t *own = _own_identity();
    if (s->bonding && own != NULL) {
        bonding_record_t record;
        memset(&record, 0, sizeof(record));
        record.flags = BONDING_IN_USE | (s->have_peer_irk ? BONDING_PEER_IRK : 0);
        record.role = c->is_central ? BONDING_ROLE_CENTRAL : BONDING_ROLE_PERIPHERAL;
        record.own = *own;
        record.peer = s->have_peer_identity ? s->peer_identity : c->addr;
        memcpy(record.peer_irk, s->peer_irk, sizeof(record.peer_irk));
        memcpy(record.ltk, s->stk, sizeof(record.ltk));     // our LTK by now (_send_keys)
        record.ediv = s->ediv;
        memcpy(record.rand, s->rand, sizeof(record.rand));
        record.key_size = s->key_size;
        _snapshot_cccds(&record);
        // In place of the bond the link had when this pairing began (checked then: the keys
        // given since are new).
        bonding_record_t *replaces = s->bond >= 0 ? &bonding_store()->records[s->bond] : NULL;
        s->bond = (int8_t)bonding_add(&record, replaces);
        bonding_save();
    }
    s->state = SMP_IDLE;
    s->security_requested = false;
}

// Encrypted with the STK: our keys, the responder's, go first.
static void _send_keys(bleio_connection_internal_t *c) {
    smp_conn_t *s = &c->smp;
    s->ediv = 0;
    memset(s->rand, 0, sizeof(s->rand));
    if (s->send_keys & SMP_DIST_ENC_KEY) {
        // The LTK of the agreed size, and the EDIV and Rand the peer asks for it with later.
        uint8_t ediv_rand[10];
        if (!_random(s->stk, 16) || !_random(ediv_rand, sizeof(ediv_rand))) {
            _fail(c, SMP_ERR_UNSPECIFIED);
            return;
        }
        memset(s->stk + s->key_size, 0, 16 - s->key_size);
        ediv_rand[0] |= 0x01;           // never 0 and 0: that is the STK's
        s->ediv = (uint16_t)(ediv_rand[0] | ediv_rand[1] << 8);
        memcpy(s->rand, ediv_rand + 2, 8);
        uint8_t info[17] = { SMP_ENCRYPTION_INFORMATION };
        memcpy(info + 1, s->stk, 16);
        _send(c, info, sizeof(info));
        uint8_t ident[11] = { SMP_CENTRAL_IDENTIFICATION };
        memcpy(ident + 1, ediv_rand, sizeof(ediv_rand));
        _send(c, ident, sizeof(ident));
    }
    const bt_addr_le_t *own = _own_identity();
    if ((s->send_keys & SMP_DIST_ID_KEY) && own != NULL) {
        uint8_t info[17] = { SMP_IDENTITY_INFORMATION };
        if (!bleio_hci_privacy_irk(info + 1)) {
            _fail(c, SMP_ERR_UNSPECIFIED);
            return;
        }
        _send(c, info, sizeof(info));
        uint8_t address[8] = { SMP_IDENTITY_ADDRESS_INFORMATION, own->type };
        memcpy(address + 2, own->a.val, 6);
        _send(c, address, sizeof(address));
    }
    s->state = SMP_WAIT_KEYS;
    if (s->take_keys == 0) {
        _complete(c);
    }
}

static void _identity(bleio_connection_internal_t *c, const uint8_t *pdu, size_t len) {
    smp_conn_t *s = &c->smp;
    if (s->state != SMP_WAIT_KEYS || !(s->take_keys & SMP_DIST_ID_KEY)) {
        _fail(c, SMP_ERR_UNSPECIFIED);
        return;
    }
    if (pdu[0] == SMP_IDENTITY_INFORMATION && len == 17) {
        memcpy(s->peer_irk, pdu + 1, 16);
        s->have_peer_irk = true;
    } else if (pdu[0] == SMP_IDENTITY_ADDRESS_INFORMATION && len == 8 && s->have_peer_irk &&
               pdu[1] <= BT_ADDR_LE_RANDOM) {
        s->peer_identity.type = pdu[1];
        memcpy(s->peer_identity.a.val, pdu + 2, 6);
        s->have_peer_identity = true;
        s->take_keys &= ~SMP_DIST_ID_KEY;
        if (s->take_keys == 0) {
            _complete(c);
        }
    } else {
        _fail(c, SMP_ERR_INVALID_PARAMS);
    }
}

static void _pdu(bleio_connection_internal_t *c, const uint8_t *pdu, size_t len) {
    smp_conn_t *s = &c->smp;
    switch (pdu[0]) {
        case SMP_PAIRING_REQUEST:
            _pairing_request(c, pdu, len);
            break;
        case SMP_PAIRING_CONFIRM:
            _pairing_confirm(c, pdu, len);
            break;
        case SMP_PAIRING_RANDOM:
            _pairing_random(c, pdu, len);
            break;
        case SMP_PAIRING_FAILED:
            // The peer gave up: no bond, and nothing is answered.
            s->state = SMP_IDLE;
            s->result = len >= 2 && pdu[1] != 0 ? pdu[1] : SMP_ERR_UNSPECIFIED;
            s->security_requested = false;
            break;
        case SMP_IDENTITY_INFORMATION:
        case SMP_IDENTITY_ADDRESS_INFORMATION:
            _identity(c, pdu, len);
            break;
        case SMP_ENCRYPTION_INFORMATION:
        case SMP_CENTRAL_IDENTIFICATION:
        case SMP_SIGNING_INFORMATION:
            // Keys we did not ask for.
            _fail(c, SMP_ERR_UNSPECIFIED);
            break;
        case SMP_SECURITY_REQUEST:
            // Only a peripheral sends it, to its central: not to us.
            break;
        default:
            // Secure Connections' PDUs among them: the Pairing Response said SC 0.
            _fail(c, SMP_ERR_CMD_NOT_SUPPORTED);
            break;
    }
}

// ---- Encryption ----

static void _ltk_request(bleio_connection_internal_t *c) {
    smp_conn_t *s = &c->smp;
    static const uint8_t zero[8] = { 0 };
    const bool zero_ediv_rand = s->ltk_ediv == 0 && memcmp(s->ltk_rand, zero, 8) == 0;
    if (s->state == SMP_WAIT_ENCRYPT) {
        // While pairing, only the STK: never a bond's key.
        if (zero_ediv_rand) {
            c->key_size = s->key_size;
            hci_le_ltk_request_reply(c->conn_handle, s->stk);
        } else {
            hci_le_ltk_request_negative_reply(c->conn_handle);
        }
        return;
    }
    const bt_addr_le_t *own = _own_identity();
    bonding_record_t *record = zero_ediv_rand || own == NULL ? NULL
        : bonding_find_ediv(own, c->is_central ? BONDING_ROLE_CENTRAL : BONDING_ROLE_PERIPHERAL,
        s->ltk_ediv, s->ltk_rand);
    if (record == NULL) {
        // The peer has a key we do not (we erased the bond, or it is another board's): it hears
        // 0x06 PIN or Key Missing.
        s->result = SMP_RESULT_KEY_MISSING;
        s->security_requested = false;
        hci_le_ltk_request_negative_reply(c->conn_handle);
        return;
    }
    s->bond = (int8_t)(record - bonding_store()->records);
    memcpy(s->stk, record->ltk, sizeof(s->stk));
    s->ediv = record->ediv;
    c->key_size = record->key_size;
    c->ediv = record->ediv;
    bonding_touch(record);
    hci_le_ltk_request_reply(c->conn_handle, record->ltk);
}

static void _encryption(bleio_connection_internal_t *c) {
    smp_conn_t *s = &c->smp;
    if (s->enc_status != 0) {
        s->result = SMP_RESULT_ENCRYPTION;
        s->state = SMP_IDLE;
        s->security_requested = false;
        return;
    }
    if (!c->encrypted) {
        return;
    }
    if (s->state == SMP_WAIT_ENCRYPT) {
        _send_keys(c);
        return;
    }
    bonding_record_t *record = _link_bond(c);
    if (record != NULL && !s->refresh) {
        _restore_cccds(c, record);
    }
    s->security_requested = false;
}

// ---- What the handlers record ----

void smp_on_connect(uint16_t conn_handle, const bt_addr_le_t *own) {
    bleio_connection_internal_t *c = _connection(conn_handle);
    if (c == NULL) {
        return;
    }
    memset(&c->smp, 0, sizeof(c->smp));
    c->smp.bond = -1;
    c->smp.own = *own;
    c->smp.pending = PENDING_CONNECTED;
}

void smp_on_pdu(uint16_t conn_handle, const uint8_t *pdu, uint16_t len) {
    bleio_connection_internal_t *c = _connection(conn_handle);
    if (c == NULL || len == 0) {
        return;
    }
    smp_conn_t *s = &c->smp;
    if (s->queue_count == SMP_QUEUE) {
        return;     // a peer that does not wait for answers: dropped, and the pairing times out
    }
    uint8_t slot = (s->queue_head + s->queue_count) % SMP_QUEUE;
    // A PDU longer than any of legacy pairing is kept as far as it fits: it is refused anyway.
    s->queue_len[slot] = len < SMP_PDU_MAX ? len : SMP_PDU_MAX;
    memcpy(s->queue[slot], pdu, s->queue_len[slot]);
    s->queue_count++;
}

void smp_on_ltk_request(uint16_t conn_handle, uint16_t ediv, const uint8_t rand[8]) {
    bleio_connection_internal_t *c = _connection(conn_handle);
    if (c == NULL) {
        return;
    }
    c->smp.ltk_ediv = ediv;
    memcpy(c->smp.ltk_rand, rand, 8);
    c->smp.pending |= PENDING_LTK;
}

void smp_on_encryption(uint16_t conn_handle, uint8_t status, bool on) {
    bleio_connection_internal_t *c = _connection(conn_handle);
    if (c == NULL) {
        return;
    }
    // A key refresh of a link encrypted already (or a second event before the background ran).
    c->smp.refresh = c->encrypted && on;
    c->encrypted = status == 0 && on;
    c->smp.enc_status = status;
    c->smp.pending |= PENDING_ENCRYPTION;
}

void smp_on_cccd_written(uint16_t conn_handle) {
    bleio_connection_internal_t *c = _connection(conn_handle);
    if (c != NULL && c->encrypted && c->smp.bond >= 0) {
        c->do_bond_cccds = true;
        c->do_bond_cccds_request_time = supervisor_ticks_ms64();
    }
}

void smp_on_disconnect(uint16_t conn_handle) {
    bleio_connection_internal_t *c = _connection(conn_handle);
    if (c == NULL) {
        return;
    }
    // CCCD changes not yet in the bond go in now: the CCCDs are cleared as the link goes.
    bonding_record_t *record = c->do_bond_cccds ? _link_bond(c) : NULL;
    if (record != NULL) {
        _snapshot_cccds(record);
        smp_save_pending = true;
    }
    c->do_bond_cccds = false;
    c->smp.state = SMP_IDLE;
    c->smp.pending = 0;
    c->smp.queue_count = 0;
    c->smp.security_requested = false;
}

void smp_reset(void) {
    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        memset(&bleio_connections[i].smp, 0, sizeof(bleio_connections[i].smp));
        bleio_connections[i].smp.bond = -1;
        bleio_connections[i].do_bond_cccds = false;
    }
}

// ---- The background ----

static void _connection_background(bleio_connection_internal_t *c) {
    smp_conn_t *s = &c->smp;
    const uint16_t handle = c->conn_handle;
    if (s->pending & PENDING_CONNECTED) {
        s->pending &= ~PENDING_CONNECTED;
        // A peer we have a bond with is asked to encrypt at once, so a central that does not by
        // itself still gets its CCCDs back; an unknown one is never pushed to pair.
        const bt_addr_le_t *own = _own_identity();
        if (!c->is_central && own != NULL && bonding_find_peer(own, &c->addr) != NULL) {
            const uint8_t pdu[2] = { SMP_SECURITY_REQUEST, SMP_AUTH_BONDING };
            _send(c, pdu, sizeof(pdu));
        }
    }
    if (s->pending & PENDING_LTK) {
        s->pending &= ~PENDING_LTK;
        _ltk_request(c);
    }
    while (s->queue_count > 0 && c->conn_handle == handle) {
        uint8_t pdu[SMP_PDU_MAX];
        const uint8_t len = s->queue_len[s->queue_head];
        memcpy(pdu, s->queue[s->queue_head], len);
        s->queue_head = (s->queue_head + 1) % SMP_QUEUE;
        s->queue_count--;
        s->last_pdu_ms = supervisor_ticks_ms64();
        _pdu(c, pdu, len);
    }
    if (c->conn_handle != handle) {
        return;
    }
    if (s->pending & PENDING_ENCRYPTION) {
        s->pending &= ~PENDING_ENCRYPTION;
        _encryption(c);
    }
    const uint64_t now = supervisor_ticks_ms64();
    if ((s->state != SMP_IDLE || s->security_requested) && now - s->last_pdu_ms > SMP_TIMEOUT_MS) {
        // The SMP timeout: the pairing is over, with no bond.
        s->state = SMP_IDLE;
        s->security_requested = false;
        s->result = SMP_RESULT_TIMEOUT;
    }
    if (c->do_bond_cccds && now - c->do_bond_cccds_request_time >= SMP_CCCD_SAVE_MS) {
        c->do_bond_cccds = false;
        bonding_record_t *record = _link_bond(c);
        if (record != NULL) {
            _snapshot_cccds(record);
            bonding_save();
        }
    }
}

void smp_background(void) {
    // Not inside a command's wait (its answer would be taken for this one's), nor nested.
    if (smp_background_running || hci_busy()) {
        return;
    }
    smp_background_running = true;
    if (smp_save_pending) {
        smp_save_pending = false;
        bonding_save();
    }
    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        bleio_connection_internal_t *c = &bleio_connections[i];
        if (c->conn_handle != BLE_CONN_HANDLE_INVALID) {
            _connection_background(c);
        }
    }
    smp_background_running = false;
}

// ---- Connection.pair() ----

static const char *_reason(uint8_t result) {
    switch (result) {
        case 0x01:
            return "passkey entry failed";
        case 0x02:
            return "OOB not available";
        case 0x03:
            return "authentication requirements";
        case SMP_ERR_CONFIRM_VALUE:
            return "confirm value failed";
        case SMP_ERR_PAIRING_NOT_SUPPORTED:
            return "pairing not supported";
        case SMP_ERR_ENC_KEY_SIZE:
            return "encryption key size";
        case SMP_ERR_CMD_NOT_SUPPORTED:
            return "command not supported";
        case 0x09:
            return "repeated attempts";
        case SMP_ERR_INVALID_PARAMS:
            return "invalid parameters";
        case SMP_RESULT_KEY_MISSING:
            return "the peer's key is not in the bond store";
        case SMP_RESULT_ENCRYPTION:
            return "encryption failed";
        default:
            return "unspecified reason";
    }
}

void smp_pair(bleio_connection_internal_t *c, bool bond) {
    if (c->is_central) {
        // The central's side of pairing comes with the program's own connections.
        mp_raise_NotImplementedError(NULL);
    }
    smp_conn_t *s = &c->smp;
    const uint16_t handle = c->conn_handle;
    if (c->encrypted && s->state == SMP_IDLE) {
        return;
    }
    s->result = SMP_RESULT_NONE;
    s->no_bond = !bond;
    if (s->state == SMP_IDLE) {
        // A Security Request: the central pairs, or encrypts with a bond it has.
        const uint8_t pdu[2] = { SMP_SECURITY_REQUEST, bond ? SMP_AUTH_BONDING : 0 };
        _send(c, pdu, sizeof(pdu));
        s->security_requested = true;
    }
    uint8_t result = SMP_RESULT_NONE;
    while (c->conn_handle == handle && !mp_hal_is_interrupted()) {
        if (c->encrypted && s->state == SMP_IDLE && !(s->pending & PENDING_ENCRYPTION)) {
            break;
        }
        if (s->result != SMP_RESULT_NONE) {
            result = s->result;
            break;
        }
        // A short sleep, not a spin: the background runs meanwhile, and a lab's fast time moves on.
        mp_hal_delay_ms(10);
    }
    s->no_bond = false;
    if (mp_hal_is_interrupted()) {
        return;
    }
    if (c->conn_handle != handle) {
        mp_raise_ConnectionError(MP_ERROR_TEXT("Not connected"));
    }
    if (result == SMP_RESULT_TIMEOUT) {
        mp_raise_bleio_SecurityError(MP_ERROR_TEXT("Pairing timed out"));
    }
    if (result != SMP_RESULT_NONE) {
        mp_raise_bleio_SecurityError(MP_ERROR_TEXT("Pairing failed: %s (0x%02x)"), _reason(result), result);
    }
}

// ---- Privacy ----

bool bleio_hci_privacy_irk(uint8_t irk[16]) {
    bonding_store_t *store = bonding_store();
    if (!(store->header.flags & BONDING_HAVE_IRK)) {
        if (!_random(store->header.irk, sizeof(store->header.irk))) {
            return false;
        }
        store->header.flags |= BONDING_HAVE_IRK;
        bonding_save();
    }
    memcpy(irk, store->header.irk, 16);
    return true;
}

bool bleio_hci_privacy_new_rpa(uint8_t addr[6]) {
    uint8_t irk[16];
    if (!bleio_hci_privacy_irk(irk) || !_random(addr + 3, 3)) {
        return false;
    }
    // prand: its two top bits 01, the rest random; the hash of it below.
    addr[5] = (addr[5] & 0x3f) | 0x40;
    smp_ah(irk, addr + 3, addr);
    return true;
}
