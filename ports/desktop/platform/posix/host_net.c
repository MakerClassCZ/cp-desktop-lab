// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "host_net.h"

#include <arpa/inet.h>
#include <fcntl.h>
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
#ifdef __APPLE__
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <sys/sysctl.h>
#endif

#ifdef __APPLE__
// The default route's gateway through the interface named, from the kernel's routing table:
// each message is a header and its addresses, the destination first, the gateway next, each
// padded to 4 bytes (an empty one, all zeros, takes 4).
static uint32_t _gateway(const char *name) {
    int mib[] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY };
    size_t len = 0;
    if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0 || len == 0) {
        return 0;
    }
    char *buf = malloc(len);
    uint32_t gateway = 0;
    if (buf != NULL && sysctl(mib, 6, buf, &len, NULL, 0) == 0) {
        for (char *p = buf; p < buf + len && gateway == 0; p += ((struct rt_msghdr *)p)->rtm_msglen) {
            struct rt_msghdr *rtm = (struct rt_msghdr *)p;
            if ((rtm->rtm_addrs & (RTA_DST | RTA_GATEWAY)) != (RTA_DST | RTA_GATEWAY)) {
                continue;
            }
            struct sockaddr *dst = (struct sockaddr *)(rtm + 1);
            struct sockaddr *via = (struct sockaddr *)((char *)dst + (dst->sa_len > 0 ? (dst->sa_len + 3) & ~3 : 4));
            bool default_route = dst->sa_len == 0 || ((struct sockaddr_in *)dst)->sin_addr.s_addr == 0;
            char itf[IF_NAMESIZE];
            if (default_route && via->sa_family == AF_INET && if_indextoname(rtm->rtm_index, itf) != NULL
                && strcmp(itf, name) == 0) {
                gateway = ((struct sockaddr_in *)via)->sin_addr.s_addr;
            }
        }
    }
    free(buf);
    return gateway;
}
#endif

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
    #ifdef __APPLE__
    info->gateway = _gateway(name);
    FILE *f = NULL;
    #else
    // The default route's gateway: /proc/net/route prints addresses as they lie in memory.
    FILE *f = fopen("/proc/net/route", "r");
    #endif
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
    #ifdef __APPLE__
    // The interface's hardware address is its link-level address.
    if (name[0] != '\0' && getifaddrs(&list) == 0) {
        for (struct ifaddrs *a = list; a != NULL; a = a->ifa_next) {
            struct sockaddr_dl *link = (struct sockaddr_dl *)a->ifa_addr;
            if (link != NULL && link->sdl_family == AF_LINK && link->sdl_alen == 6 && strcmp(a->ifa_name, name) == 0) {
                memcpy(info->mac, LLADDR(link), 6);
                info->have_mac = true;
            }
        }
        freeifaddrs(list);
    }
    #else
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
    #endif
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

// The Internet checksum of an ICMP message.
static uint16_t _checksum(const void *data, size_t len) {
    const uint16_t *word = data;
    uint32_t sum = 0;
    for (; len > 1; len -= 2) {
        sum += *word++;
    }
    sum = (sum >> 16) + (sum & 0xffff);
    sum += sum >> 16;
    return (uint16_t) ~sum;
}

int host_ping_ms(uint32_t address, int timeout_ms) {
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr.s_addr = address };
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    // An unprivileged ICMP socket, where net.ipv4.ping_group_range lets the user have one.
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (s >= 0) {
        // The BSD header, which both systems have; Linux fills the checksum in itself, macOS not.
        struct icmp echo = { .icmp_type = ICMP_ECHO, .icmp_seq = htons(1) };
        echo.icmp_cksum = _checksum(&echo, ICMP_MINLEN);
        if (sendto(s, &echo, ICMP_MINLEN, 0, (struct sockaddr *)&to, sizeof(to)) == ICMP_MINLEN) {
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
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    fcntl(s, F_SETFL, O_NONBLOCK);
    to.sin_port = htons(80);
    connect(s, (struct sockaddr *)&to, sizeof(to));
    struct pollfd pfd = { .fd = s, .events = POLLOUT };
    int ready = poll(&pfd, 1, timeout_ms);
    close(s);
    return ready == 1 ? (int)_ms_since(&start) : -1;
}
