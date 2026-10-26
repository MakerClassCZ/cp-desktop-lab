// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "host_net.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

bool host_net_info(host_net_info_t *info) {
    memset(info, 0, sizeof(*info));
    if (!host_net_route_address(&info->address)) {
        return false;
    }
    char name[64] = "";
    struct ifaddrs *list;
    if (getifaddrs(&list) == 0) {
        for (struct ifaddrs *a = list; a != NULL; a = a->ifa_next) {
            if (a->ifa_addr != NULL && a->ifa_addr->sa_family == AF_INET
                && ((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr == info->address) {
                snprintf(name, sizeof(name), "%s", a->ifa_name);
                if (a->ifa_netmask != NULL) {
                    info->netmask = ((struct sockaddr_in *)a->ifa_netmask)->sin_addr.s_addr;
                }
            }
        }
        freeifaddrs(list);
    }
    char line[256];
    // The default route's gateway: /proc/net/route prints addresses as they lie in memory.
    FILE *f = fopen("/proc/net/route", "r");
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        char itf[64];
        unsigned long dest, gateway;
        if (sscanf(line, "%63s %lx %lx", itf, &dest, &gateway) == 3 && dest == 0 && strcmp(itf, name) == 0) {
            info->gateway = (uint32_t)gateway;
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    f = fopen("/etc/resolv.conf", "r");
    while (f != NULL && info->dns == 0 && fgets(line, sizeof(line), f) != NULL) {
        char server[64];
        if (sscanf(line, "nameserver %63s", server) == 1) {
            inet_pton(AF_INET, server, &info->dns);
        }
    }
    if (f != NULL) {
        fclose(f);
    }
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", name);
    f = name[0] != '\0' ? fopen(path, "r") : NULL;
    unsigned m[6];
    if (f != NULL && fscanf(f, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            info->mac[i] = (uint8_t)m[i];
        }
        info->have_mac = true;
    }
    if (f != NULL) {
        fclose(f);
    }
    return true;
}

// nmcli's terse fields are split by ':', a ':' in a value escaped as '\:'.
static char *_field(char **line) {
    char *start = *line, *out = *line;
    for (char *c = *line; *c != '\0' && *c != '\n'; c++) {
        if (*c == '\\' && c[1] != '\0') {
            *out++ = *++c;
        } else if (*c == ':') {
            *out = '\0';
            *line = c + 1;
            return start;
        } else {
            *out++ = *c;
        }
    }
    *out = '\0';
    *line = out;
    return start;
}

bool host_wifi_current(host_wifi_t *wifi) {
    FILE *p = popen("nmcli -t -f active,ssid,signal,chan,bssid dev wifi 2>/dev/null", "r");
    if (p == NULL) {
        return false;
    }
    char line[256];
    bool found = false;
    while (!found && fgets(line, sizeof(line), p) != NULL) {
        char *rest = line;
        char *active = _field(&rest);
        char *ssid = _field(&rest);
        char *signal = _field(&rest);
        char *channel = _field(&rest);
        char *bssid = _field(&rest);
        if (strcmp(active, "yes") != 0 || ssid[0] == '\0') {
            continue;
        }
        snprintf(wifi->ssid, sizeof(wifi->ssid), "%s", ssid);
        wifi->rssi = atoi(signal) / 2 - 100;
        wifi->channel = atoi(channel);
        unsigned m[6] = { 0 };
        sscanf(bssid, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
        for (int i = 0; i < 6; i++) {
            wifi->bssid[i] = (uint8_t)m[i];
        }
        found = true;
    }
    pclose(p);
    return found;
}

static long _ms_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

int host_ping_ms(uint32_t address, int timeout_ms) {
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr.s_addr = address };
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    // An unprivileged ICMP socket, where net.ipv4.ping_group_range lets the user have one.
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (s >= 0) {
        struct icmphdr echo = { .type = ICMP_ECHO };
        echo.un.echo.sequence = htons(1);
        if (sendto(s, &echo, sizeof(echo), 0, (struct sockaddr *)&to, sizeof(to)) == (ssize_t)sizeof(echo)) {
            struct pollfd pfd = { .fd = s, .events = POLLIN };
            char reply[64];
            if (poll(&pfd, 1, timeout_ms) == 1 && recv(s, reply, sizeof(reply), 0) > 0) {
                close(s);
                return (int)_ms_since(&start);
            }
        }
        close(s);
        return -1;
    }
    // Else a TCP connection: an answer, even a refusal, means the host is there.
    s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s < 0) {
        return -1;
    }
    to.sin_port = htons(80);
    connect(s, (struct sockaddr *)&to, sizeof(to));
    struct pollfd pfd = { .fd = s, .events = POLLOUT };
    int ready = poll(&pfd, 1, timeout_ms);
    close(s);
    return ready == 1 ? (int)_ms_since(&start) : -1;
}

