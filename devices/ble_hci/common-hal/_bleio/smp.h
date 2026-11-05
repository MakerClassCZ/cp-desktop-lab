// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

// The Security Manager (Core Vol 3 Part H): LE Legacy pairing, "Just Works", as the responder (we
// are the peripheral, the peer pairs), bonds kept in the bond store, and encryption with them.
//
// The HCI handlers only record what came (smp_on_*): a handler cannot send a command and wait for
// its answer, since the answer is read by the same poll. smp_background(), after the poll, does
// the crypto and sends the PDUs and the commands.

#include <stdbool.h>
#include <stdint.h>

#include "hci_include/addr.h"

// SMP's PDUs of LE Legacy pairing are at most 17 bytes; room for more is kept for a peer that
// sends longer ones, which are refused.
#define SMP_PDU_MAX 24
#define SMP_QUEUE 4

// What a pairing ended with, besides a Pairing Failed reason (0x01-0x0e).
#define SMP_RESULT_NONE 0x00
#define SMP_RESULT_TIMEOUT 0xf0     // 30 s without the next PDU
#define SMP_RESULT_KEY_MISSING 0xf1 // the peer encrypted with a key we have no bond for
#define SMP_RESULT_ENCRYPTION 0xf2  // Encryption Change with an error

// A link's SMP: its pairing, the keys of a bond made or used, the work for the background.
typedef struct {
    uint8_t state;                  // where a pairing is (smp.c)
    uint8_t pending;                // what the handlers recorded for smp_background()
    uint8_t result;                 // SMP_RESULT_*, or the reason a pairing failed
    bool bonding;                   // this pairing makes a bond
    bool no_bond;                   // pair(bond=False) is waiting: make none
    bool security_requested;        // we sent a Security Request and wait for the peer
    int8_t bond;                    // the link's bond in the store, -1 for none
    uint8_t key_size;               // agreed by this pairing
    uint8_t send_keys;              // the keys we distribute (SMP's key distribution bits)
    uint8_t take_keys;              // the keys the peer is still to distribute
    bt_addr_le_t own;               // our address on the link, as c1 takes it
    uint8_t preq[7];
    uint8_t prsp[7];
    uint8_t confirm[16];            // the peer's
    uint8_t random[16];             // ours (Srand), then the peer's
    uint8_t stk[16];                // the STK, then the LTK we give
    uint16_t ediv;                  // and the EDIV and Rand it is asked for with
    uint8_t rand[8];
    uint8_t peer_irk[16];
    bt_addr_le_t peer_identity;
    bool have_peer_irk;
    bool have_peer_identity;
    uint8_t enc_status;             // the Encryption Change's
    bool refresh;                   // ... on a link encrypted already
    uint16_t ltk_ediv;              // the LE Long Term Key Request waiting for an answer
    uint8_t ltk_rand[8];
    uint64_t last_pdu_ms;           // for the 30 s SMP timeout
    uint8_t queue[SMP_QUEUE][SMP_PDU_MAX];  // PDUs from the peer, oldest first
    uint8_t queue_len[SMP_QUEUE];
    uint8_t queue_head;
    uint8_t queue_count;
} smp_conn_t;

struct _bleio_connection_internal;

// A link opened: its SMP reset; own is our address on it.
void smp_on_connect(uint16_t conn_handle, const bt_addr_le_t *own);
// A link closing: a bonded peer's CCCD changes are taken into its bond before they are cleared.
void smp_on_disconnect(uint16_t conn_handle);
// An SMP PDU from the peer (L2CAP channel 6).
void smp_on_pdu(uint16_t conn_handle, const uint8_t *pdu, uint16_t len);
// LE Long Term Key Request: the peer starts encryption with the key of ediv and rand.
void smp_on_ltk_request(uint16_t conn_handle, uint16_t ediv, const uint8_t rand[8]);
// Encryption Change or Encryption Key Refresh Complete.
void smp_on_encryption(uint16_t conn_handle, uint8_t status, bool on);
// A peer wrote a CCCD: a bonded peer's are saved in its bond, a moment later.
void smp_on_cccd_written(uint16_t conn_handle);
// The work the handlers recorded, done outside them; nothing while an HCI command is waited for.
void smp_background(void);
// The SMP state of every link cleared, as at a reset.
void smp_reset(void);

// Connection.pair() as the peripheral: a Security Request, then the wait for the peer's pairing
// or encryption. Raises SecurityError or ConnectionError.
void smp_pair(struct _bleio_connection_internal *connection, bool bond);

// Our identity resolving key, made once (LE Rand) and kept in the bond store; false when the
// controller gave no random bytes.
bool bleio_hci_privacy_irk(uint8_t irk[16]);
// A new resolvable private address of our IRK, least significant byte first; false as above.
bool bleio_hci_privacy_new_rpa(uint8_t addr[6]);
