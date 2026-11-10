// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>

// The network's faults (fault.h) for both of the lab's networks, wifi's sockets on the computer
// and the AirLift's: each side asks here and gives the answer the way its board would. Errors
// are positive errno values (py/mperrno.h). All on the VM thread.

// The Wi-Fi link. Each network call asks first: "fault wifi.link drop" takes the link down
// there, and every connection made before it is reset. It stays down until the radio connects
// again or the run ends (net_link_restore). net_link_is_up only looks: for what the supervisor
// asks too (the status bar's address), which must not fire a rule meant for the program.
bool net_link_up(void);
bool net_link_is_up(void);
void net_link_restore(void);

// A connection, as its side keeps it.
typedef struct {
    char host[64];          // the peer as the program named it, or its address
    long port;
    long send_left;         // bytes before a cut, -1: no cut
    long recv_left;
    bool reset, stalled;
    unsigned link;          // the link it was made on
} net_conn_t;

// Opened: no fault on it yet.
void net_conn_open(net_conn_t *c, const char *host, long port);

// A lookup: false when it fails (net.dns fail, the link down).
bool net_dns(const char *host);
// A socket for the program: false when the board is out of them (net.socket full, or open
// sockets at the limit; open is how many the program has now, limit 0 for none).
bool net_socket(size_t open, size_t limit);
// A connect: 0, or the error it ends with: ECONNREFUSED, ETIMEDOUT (after the caller's
// timeout), EHOSTUNREACH (the link down).
int net_connect(const char *host, long port);

// A send of len bytes: how many go (fewer for a cut), or -errno. When a cut has used its bytes
// the connection is reset (c->reset): the side ends its own so the peer sees the end.
long net_send(net_conn_t *c, size_t len);

// Before a receive: 0 to read, else what it gives instead: -ECONNRESET, NET_END (a cut has
// given its bytes), NET_STALL (nothing comes: as from a silent peer).
#define NET_END (-1000)
#define NET_STALL (-1001)
int net_recv_check(net_conn_t *c);
// got bytes came: how many of them the program gets (a fault may cut or drop them).
size_t net_recv(net_conn_t *c, size_t got);
