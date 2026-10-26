// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the computer's network looks like, for wifi.radio: the interface its default route takes.
// Addresses are IPv4 in network byte order, as ipaddress takes them.

typedef struct {
    uint32_t address, netmask, gateway, dns;
    uint8_t mac[6];
    bool have_mac;
} host_net_info_t;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int rssi;           // dBm
    int channel;
} host_wifi_t;

// False when the computer has no route out.
bool host_net_info(host_net_info_t *info);
// The Wi-Fi network the computer is on; false on Ethernet, or when it cannot be asked.
bool host_wifi_current(host_wifi_t *wifi);
// The round trip to an address in milliseconds, or -1.
int host_ping_ms(uint32_t address, int timeout_ms);
// The computer's name.
void host_hostname(char *name, size_t len);

// For each system's file: the local address the default route takes, false when there is none.
bool host_net_route_address(uint32_t *address);
