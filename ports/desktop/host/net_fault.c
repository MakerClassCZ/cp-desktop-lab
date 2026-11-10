// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "net_fault.h"

#include <stdio.h>
#include <string.h>

#include "py/mperrno.h"

#include "bus_trace.h"
#include "fault.h"

static bool _down;
static unsigned _link;      // a drop starts a new one

bool net_link_up(void) {
    if (!_down && fault_check("wifi.link", NULL, -1).fired) {
        _down = true;
        _link++;
    }
    return !_down;
}

bool net_link_is_up(void) {
    return !_down;
}

void net_link_restore(void) {
    _down = false;
}

void net_conn_open(net_conn_t *c, const char *host, long port) {
    memset(c, 0, sizeof(*c));
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->port = port;
    c->send_left = c->recv_left = -1;
    c->link = _link;
}

// A connection the link took down with it is reset.
static bool _alive(net_conn_t *c) {
    if (!net_link_up() || c->link != _link) {
        c->reset = true;
    }
    return !c->reset;
}

bool net_dns(const char *host) {
    return net_link_up() && !fault_check("net.dns", host, -1).fired;
}

bool net_socket(size_t open, size_t limit) {
    if (fault_check("net.socket", NULL, -1).fired) {
        return false;
    }
    if (limit > 0 && open >= limit) {
        bus_trace_text("NET out of sockets: %u open", (unsigned)open);
        return false;
    }
    return true;
}

int net_connect(const char *host, long port) {
    if (!net_link_up()) {
        return MP_EHOSTUNREACH;
    }
    fault_t f = fault_check("net.connect", host, port);
    return !f.fired ? 0 : strcmp(f.action, "refuse") == 0 ? MP_ECONNREFUSED : MP_ETIMEDOUT;
}

// Up to len of a cut's bytes left; at none the connection is reset.
static size_t _budget(long *left, size_t len) {
    size_t k = (size_t)*left < len ? (size_t)*left : len;
    *left -= (long)k;
    return k;
}

long net_send(net_conn_t *c, size_t len) {
    if (!_alive(c)) {
        return -MP_ECONNRESET;
    }
    if (c->send_left < 0) {
        fault_t f = fault_check("net.send", c->host, c->port);
        if (f.fired && strcmp(f.action, "error") == 0) {
            return -MP_EIO;
        }
        if (f.fired) {
            c->send_left = f.arg;
        }
    }
    if (c->send_left < 0) {
        return (long)len;
    }
    size_t k = _budget(&c->send_left, len);
    if (c->send_left == 0) {
        c->reset = true;
    }
    return k == 0 ? -MP_ECONNRESET : (long)k;
}

int net_recv_check(net_conn_t *c) {
    if (!_alive(c)) {
        return -MP_ECONNRESET;
    }
    return c->stalled ? NET_STALL : c->recv_left == 0 ? NET_END : 0;
}

size_t net_recv(net_conn_t *c, size_t got) {
    if (c->recv_left < 0) {
        fault_t f = fault_check("net.recv", c->host, c->port);
        if (f.fired && strcmp(f.action, "reset") == 0) {
            c->reset = true;
            return 0;
        }
        if (f.fired && strcmp(f.action, "stall") == 0) {
            c->stalled = true;
            return 0;
        }
        if (f.fired) {
            c->recv_left = f.arg;
        }
    }
    return c->recv_left < 0 ? got : _budget(&c->recv_left, got);
}
