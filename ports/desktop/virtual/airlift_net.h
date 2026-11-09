// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The AirLift's network: nina-fw's ten sockets over the computer's. What waits on the network
// (DNS, a connect, a ping) runs on a worker thread; TLS (mbedtls, whose memory is the VM heap's)
// on the VM thread, a step at a time. Everything here is called on the VM thread.

#define AIRLIFT_SOCKETS 10
#define AIRLIFT_NO_SOCKET 255

enum { AIRLIFT_TCP = 0, AIRLIFT_UDP = 1, AIRLIFT_TLS = 2 };

// Every socket closed; a job in flight is dropped.
void airlift_net_reset(void);
uint8_t airlift_net_get_socket(void);

// Jobs: one at a time, as the ESP32 does one command at a time. airlift_net_poll() is false while
// one runs, then true with its result: a start 1 or 0, a lookup 1 or 0, a ping its ms or -1.
bool airlift_net_start(uint8_t sock, const char *host, uint32_t ip, uint16_t port, uint8_t mode);
void airlift_net_resolve(const char *host);
void airlift_net_ping(uint32_t ip, uint8_t ttl);
bool airlift_net_poll(int *result);
// The last lookup's address (network order), 0 when it failed.
uint32_t airlift_net_resolved(void);

uint8_t airlift_net_state(uint8_t sock);
// A listening socket: a new slot with the next connection, or AIRLIFT_NO_SOCKET.
uint16_t airlift_net_available(uint8_t sock);
int airlift_net_read(uint8_t sock, uint8_t *buf, uint16_t len, bool peek);
uint16_t airlift_net_write(uint8_t sock, const uint8_t *buf, uint16_t len);
void airlift_net_stop(uint8_t sock);
bool airlift_net_listen(uint8_t sock, uint16_t port, uint8_t mode);
uint8_t airlift_net_server_state(uint8_t sock);
bool airlift_net_udp_append(uint8_t sock, const uint8_t *buf, uint16_t len);
bool airlift_net_udp_send(uint8_t sock);
void airlift_net_remote(uint8_t sock, uint32_t *ip, uint16_t *port);
