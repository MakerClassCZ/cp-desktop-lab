// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "host_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <wlanapi.h>

bool host_net_info(host_net_info_t *info) {
    memset(info, 0, sizeof(*info));
    if (!host_net_route_address(&info->address)) {
        return false;
    }
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *list = NULL;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; tries++) {
        free(list);
        list = malloc(size);
        if (list == NULL) {
            return true;
        }
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, NULL, list, &size);
    }
    for (IP_ADAPTER_ADDRESSES *a = result == NO_ERROR ? list : NULL; a != NULL; a = a->Next) {
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u != NULL; u = u->Next) {
            struct sockaddr_in *in = (struct sockaddr_in *)u->Address.lpSockaddr;
            if (in->sin_family != AF_INET || in->sin_addr.s_addr != info->address) {
                continue;
            }
            uint8_t prefix = u->OnLinkPrefixLength;
            info->netmask = prefix == 0 ? 0 : htonl(0xffffffffu << (32 - prefix));
            if (a->FirstGatewayAddress != NULL) {
                info->gateway = ((struct sockaddr_in *)a->FirstGatewayAddress->Address.lpSockaddr)->sin_addr.s_addr;
            }
            for (IP_ADAPTER_DNS_SERVER_ADDRESS *d = a->FirstDnsServerAddress; d != NULL; d = d->Next) {
                struct sockaddr_in *dns = (struct sockaddr_in *)d->Address.lpSockaddr;
                if (dns->sin_family == AF_INET) {
                    info->dns = dns->sin_addr.s_addr;
                    break;
                }
            }
            if (a->PhysicalAddressLength == 6) {
                memcpy(info->mac, a->PhysicalAddress, 6);
                info->have_mac = true;
            }
        }
    }
    free(list);
    return true;
}

// wlanapi.dll at run time: a Windows without the WLAN service (a server) still runs.
typedef DWORD (WINAPI *open_fn)(DWORD, PVOID, PDWORD, PHANDLE);
typedef DWORD (WINAPI *close_fn)(HANDLE, PVOID);
typedef DWORD (WINAPI *enum_fn)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST *);
typedef DWORD (WINAPI *query_fn)(HANDLE, const GUID *, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID *, PWLAN_OPCODE_VALUE_TYPE);
typedef VOID (WINAPI *free_fn)(PVOID);

bool host_wifi_current(host_wifi_t *wifi) {
    HMODULE dll = LoadLibraryA("wlanapi.dll");
    if (dll == NULL) {
        return false;
    }
    open_fn wopen = (open_fn)(void *)GetProcAddress(dll, "WlanOpenHandle");
    close_fn wclose = (close_fn)(void *)GetProcAddress(dll, "WlanCloseHandle");
    enum_fn wenum = (enum_fn)(void *)GetProcAddress(dll, "WlanEnumInterfaces");
    query_fn wquery = (query_fn)(void *)GetProcAddress(dll, "WlanQueryInterface");
    free_fn wfree = (free_fn)(void *)GetProcAddress(dll, "WlanFreeMemory");
    bool found = false;
    HANDLE h = NULL;
    DWORD version;
    PWLAN_INTERFACE_INFO_LIST interfaces = NULL;
    if (wopen != NULL && wclose != NULL && wenum != NULL && wquery != NULL && wfree != NULL
        && wopen(2, NULL, &version, &h) == ERROR_SUCCESS && wenum(h, NULL, &interfaces) == ERROR_SUCCESS) {
        for (DWORD i = 0; !found && i < interfaces->dwNumberOfItems; i++) {
            WLAN_INTERFACE_INFO *itf = &interfaces->InterfaceInfo[i];
            if (itf->isState != wlan_interface_state_connected) {
                continue;
            }
            DWORD size;
            WLAN_CONNECTION_ATTRIBUTES *conn = NULL;
            ULONG *channel = NULL;
            if (wquery(h, &itf->InterfaceGuid, wlan_intf_opcode_current_connection, NULL, &size, (PVOID *)&conn, NULL) != ERROR_SUCCESS) {
                continue;
            }
            DOT11_SSID *ssid = &conn->wlanAssociationAttributes.dot11Ssid;
            size_t n = ssid->uSSIDLength < 32 ? ssid->uSSIDLength : 32;
            memcpy(wifi->ssid, ssid->ucSSID, n);
            wifi->ssid[n] = '\0';
            memcpy(wifi->bssid, conn->wlanAssociationAttributes.dot11Bssid, 6);
            // Signal quality 0-100 is -100 to -50 dBm.
            wifi->rssi = (int)conn->wlanAssociationAttributes.wlanSignalQuality / 2 - 100;
            wifi->channel = 0;
            if (wquery(h, &itf->InterfaceGuid, wlan_intf_opcode_channel_number, NULL, &size, (PVOID *)&channel, NULL) == ERROR_SUCCESS) {
                wifi->channel = (int)*channel;
                wfree(channel);
            }
            wfree(conn);
            found = true;
        }
    }
    if (interfaces != NULL) {
        wfree(interfaces);
    }
    if (h != NULL) {
        wclose(h, NULL);
    }
    FreeLibrary(dll);
    return found;
}

int host_ping_ms(uint32_t address, int timeout_ms) {
    HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) {
        return -1;
    }
    char data[8] = "cpython";
    uint8_t reply[sizeof(ICMP_ECHO_REPLY) + sizeof(data) + 8];
    DWORD n = IcmpSendEcho(icmp, (IPAddr)address, data, sizeof(data), NULL, reply, sizeof(reply), (DWORD)timeout_ms);
    IcmpCloseHandle(icmp);
    ICMP_ECHO_REPLY *r = (ICMP_ECHO_REPLY *)reply;
    return n > 0 && r->Status == IP_SUCCESS ? (int)r->RoundTripTime : -1;
}
