// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "host_net.h"

#include <stdio.h>
#include <string.h>

#include "host_socket.h"

// The local address the default route takes: a UDP socket "connected" anywhere sends nothing.
bool host_net_route_address(uint32_t *address) {
    int s = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return false;
    }
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(9) };
    inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);
    struct sockaddr_in me;
    socklen_t len = sizeof(me);
    bool ok = connect(s, (struct sockaddr *)&to, sizeof(to)) == 0 && getsockname(s, (struct sockaddr *)&me, &len) == 0;
    host_sock_close(s);
    if (ok) {
        *address = me.sin_addr.s_addr;
    }
    return ok && *address != 0;
}

void host_hostname(char *name, size_t len) {
    if (gethostname(name, (int)len) != 0) {
        snprintf(name, len, "circuitpython");
    }
    name[len - 1] = '\0';
}
