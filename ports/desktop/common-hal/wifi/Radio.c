// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>

#include "py/runtime.h"
#include "shared-bindings/ipaddress/IPv4Address.h"
#include "shared-bindings/ipaddress/__init__.h"
#include "shared-bindings/wifi/Network.h"
#include "shared-bindings/wifi/Radio.h"
#include "shared-bindings/wifi/ScannedNetworks.h"
#include "shared-bindings/wifi/__init__.h"
#include "supervisor/shared/settings.h"

#include "bus_trace.h"
#include "host_net.h"

// A board with CIRCUITPY_WIFI_SSID in settings.toml is online when the program starts.
static void _autoconnect(wifi_radio_obj_t *self) {
    static bool asked;
    if (self->connected || !self->enabled || asked) {
        return;
    }
    asked = true;
    char ssid[33];
    if (settings_get_str("CIRCUITPY_WIFI_SSID", ssid, sizeof(ssid)) == SETTINGS_OK && ssid[0] != '\0') {
        char password[64];
        bool secured = settings_get_str("CIRCUITPY_WIFI_PASSWORD", password, sizeof(password)) == SETTINGS_OK
            && password[0] != '\0';
        common_hal_wifi_radio_connect(self, (uint8_t *)ssid, strlen(ssid), (uint8_t *)password, secured ? strlen(password) : 0,
            0, 0, NULL, 0);
    }
}

// The computer's interface, when the radio is on a network.
static bool _info(wifi_radio_obj_t *self, host_net_info_t *info) {
    _autoconnect(self);
    return self->connected && host_net_info(info);
}

static mp_obj_t _address(uint32_t address) {
    return address != 0 ? common_hal_ipaddress_new_ipv4address(address) : mp_const_none;
}

bool common_hal_wifi_radio_get_enabled(wifi_radio_obj_t *self) {
    return self->enabled;
}

void common_hal_wifi_radio_set_enabled(wifi_radio_obj_t *self, bool enabled) {
    self->enabled = enabled;
    if (!enabled) {
        self->connected = false;
    }
}

mp_obj_t common_hal_wifi_radio_get_hostname(wifi_radio_obj_t *self) {
    const char *name = wifi_get_hostname_raw();
    (void)self;
    return mp_obj_new_str(name, strlen(name));
}

void common_hal_wifi_radio_set_hostname(wifi_radio_obj_t *self, const char *hostname) {
    snprintf(self->hostname, sizeof(self->hostname), "%s", hostname);
}

void wifi_radio_get_mac_address(wifi_radio_obj_t *self, uint8_t *mac) {
    host_net_info_t info;
    if (self->mac_set) {
        memcpy(mac, self->mac, 6);
    } else if (host_net_info(&info) && info.have_mac) {
        memcpy(mac, info.mac, 6);
    } else {
        static const uint8_t local[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
        memcpy(mac, local, 6);
    }
}

mp_obj_t common_hal_wifi_radio_get_mac_address(wifi_radio_obj_t *self) {
    uint8_t mac[6];
    wifi_radio_get_mac_address(self, mac);
    return mp_obj_new_bytes(mac, sizeof(mac));
}

void common_hal_wifi_radio_set_mac_address(wifi_radio_obj_t *self, const uint8_t *mac) {
    memcpy(self->mac, mac, 6);
    self->mac_set = true;
}

mp_obj_t common_hal_wifi_radio_get_mac_address_ap(wifi_radio_obj_t *self) {
    uint8_t mac[6];
    wifi_radio_get_mac_address(self, mac);
    mac[5]++;
    return mp_obj_new_bytes(mac, sizeof(mac));
}

void common_hal_wifi_radio_set_mac_address_ap(wifi_radio_obj_t *self, const uint8_t *mac) {
    (void)self;
    (void)mac;
}

mp_float_t common_hal_wifi_radio_get_tx_power(wifi_radio_obj_t *self) {
    return self->tx_power;
}

void common_hal_wifi_radio_set_tx_power(wifi_radio_obj_t *self, const mp_float_t power) {
    self->tx_power = power;
}

wifi_power_management_t common_hal_wifi_radio_get_power_management(wifi_radio_obj_t *self) {
    return self->power_management;
}

void common_hal_wifi_radio_set_power_management(wifi_radio_obj_t *self, wifi_power_management_t power_management) {
    self->power_management = power_management;
}

mp_obj_t common_hal_wifi_radio_start_scanning_networks(wifi_radio_obj_t *self, uint8_t start_channel, uint8_t stop_channel) {
    (void)start_channel;
    (void)stop_channel;
    if (self->current_scan != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Already scanning for wifi networks"));
    }
    wifi_scannednetworks_obj_t *scan = mp_obj_malloc(wifi_scannednetworks_obj_t, &wifi_scannednetworks_type);
    scan->network = wifi_network_current(true);
    self->current_scan = scan;
    return MP_OBJ_FROM_PTR(scan);
}

void common_hal_wifi_radio_stop_scanning_networks(wifi_radio_obj_t *self) {
    self->current_scan = NULL;
}

void common_hal_wifi_radio_start_station(wifi_radio_obj_t *self) {
    (void)self;
}

void common_hal_wifi_radio_stop_station(wifi_radio_obj_t *self) {
    self->connected = false;
}

void common_hal_wifi_radio_start_ap(wifi_radio_obj_t *self, uint8_t *ssid, size_t ssid_len, uint8_t *password, size_t password_len, uint8_t channel, uint32_t authmode, uint8_t max_connections) {
    mp_raise_NotImplementedError(MP_ERROR_TEXT("an access point needs a board's radio"));
}

void common_hal_wifi_radio_stop_ap(wifi_radio_obj_t *self) {
    (void)self;
}

bool common_hal_wifi_radio_get_ap_active(wifi_radio_obj_t *self) {
    (void)self;
    return false;
}

mp_obj_t common_hal_wifi_radio_get_stations_ap(wifi_radio_obj_t *self) {
    (void)self;
    return mp_const_none;
}

void common_hal_wifi_radio_start_dhcp_client(wifi_radio_obj_t *self, bool ipv4, bool ipv6) {
    (void)self;
    (void)ipv4;
    (void)ipv6;
}

void common_hal_wifi_radio_stop_dhcp_client(wifi_radio_obj_t *self) {
    (void)self;
}

void common_hal_wifi_radio_start_dhcp_server(wifi_radio_obj_t *self) {
    (void)self;
}

void common_hal_wifi_radio_stop_dhcp_server(wifi_radio_obj_t *self) {
    (void)self;
}

wifi_radio_error_t common_hal_wifi_radio_connect(wifi_radio_obj_t *self, uint8_t *ssid, size_t ssid_len, uint8_t *password, size_t password_len, uint8_t channel, mp_float_t timeout, uint8_t *bssid, size_t bssid_len) {
    (void)password;
    (void)channel;
    (void)timeout;
    (void)bssid;
    (void)bssid_len;
    if (!self->enabled) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Wifi is not enabled"));
    }
    // The computer is online already: any network the program names is the one it is on.
    size_t n = ssid_len < sizeof(self->ssid) - 1 ? ssid_len : sizeof(self->ssid) - 1;
    memcpy(self->ssid, ssid, n);
    self->ssid[n] = '\0';
    self->secured = password_len > 0;
    self->connected = true;
    bus_trace_text("WIFI connect %s", self->ssid);
    return WIFI_RADIO_ERROR_NONE;
}

bool common_hal_wifi_radio_get_connected(wifi_radio_obj_t *self) {
    _autoconnect(self);
    return self->connected;
}

mp_obj_t common_hal_wifi_radio_get_ap_info(wifi_radio_obj_t *self) {
    _autoconnect(self);
    return wifi_network_current(false);
}

mp_obj_t common_hal_wifi_radio_get_ipv4_dns(wifi_radio_obj_t *self) {
    host_net_info_t info;
    if (!_info(self, &info)) {
        return mp_const_none;
    }
    return _address(self->dns != 0 ? self->dns : info.dns);
}

void common_hal_wifi_radio_set_ipv4_dns(wifi_radio_obj_t *self, mp_obj_t ipv4_dns_addr) {
    mp_buffer_info_t packed;
    mp_get_buffer_raise(common_hal_ipaddress_ipv4address_get_packed(MP_OBJ_TO_PTR(ipv4_dns_addr)), &packed, MP_BUFFER_READ);
    memcpy(&self->dns, packed.buf, 4);
}

mp_obj_t common_hal_wifi_radio_get_ipv4_gateway(wifi_radio_obj_t *self) {
    host_net_info_t info;
    return _info(self, &info) ? _address(info.gateway) : mp_const_none;
}

mp_obj_t common_hal_wifi_radio_get_ipv4_gateway_ap(wifi_radio_obj_t *self) {
    (void)self;
    return mp_const_none;
}

mp_obj_t common_hal_wifi_radio_get_ipv4_subnet(wifi_radio_obj_t *self) {
    host_net_info_t info;
    return _info(self, &info) ? _address(info.netmask) : mp_const_none;
}

mp_obj_t common_hal_wifi_radio_get_ipv4_subnet_ap(wifi_radio_obj_t *self) {
    (void)self;
    return mp_const_none;
}

uint32_t wifi_radio_get_ipv4_address(wifi_radio_obj_t *self) {
    host_net_info_t info;
    return _info(self, &info) ? info.address : 0;
}

mp_obj_t common_hal_wifi_radio_get_ipv4_address(wifi_radio_obj_t *self) {
    return _address(wifi_radio_get_ipv4_address(self));
}

mp_obj_t common_hal_wifi_radio_get_ipv4_address_ap(wifi_radio_obj_t *self) {
    (void)self;
    return mp_const_none;
}

static mp_obj_t _address_tuple(uint32_t address) {
    if (address == 0) {
        return mp_const_empty_tuple;
    }
    const uint8_t *b = (const uint8_t *)&address;
    char text[16];
    snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    mp_obj_t str = mp_obj_new_str(text, strlen(text));
    return mp_obj_new_tuple(1, &str);
}

mp_obj_t common_hal_wifi_radio_get_addresses(wifi_radio_obj_t *self) {
    return _address_tuple(wifi_radio_get_ipv4_address(self));
}

mp_obj_t common_hal_wifi_radio_get_addresses_ap(wifi_radio_obj_t *self) {
    (void)self;
    return mp_const_empty_tuple;
}

mp_obj_t common_hal_wifi_radio_get_dns(wifi_radio_obj_t *self) {
    host_net_info_t info;
    if (!_info(self, &info)) {
        return mp_const_empty_tuple;
    }
    return _address_tuple(self->dns != 0 ? self->dns : info.dns);
}

void common_hal_wifi_radio_set_dns(wifi_radio_obj_t *self, mp_obj_t dns_addr) {
    (void)self;
    (void)dns_addr;
    mp_raise_NotImplementedError(MP_ERROR_TEXT("the computer's network sets its DNS"));
}

void common_hal_wifi_radio_set_ipv4_address(wifi_radio_obj_t *self, mp_obj_t ipv4, mp_obj_t netmask, mp_obj_t gateway, mp_obj_t ipv4_dns_addr) {
    (void)self;
    (void)ipv4;
    (void)netmask;
    (void)gateway;
    (void)ipv4_dns_addr;
    mp_raise_NotImplementedError(MP_ERROR_TEXT("the computer's network sets its address"));
}

void common_hal_wifi_radio_set_ipv4_address_ap(wifi_radio_obj_t *self, mp_obj_t ipv4, mp_obj_t netmask, mp_obj_t gateway) {
    (void)self;
    (void)ipv4;
    (void)netmask;
    (void)gateway;
    mp_raise_NotImplementedError(MP_ERROR_TEXT("an access point needs a board's radio"));
}

mp_int_t common_hal_wifi_radio_ping(wifi_radio_obj_t *self, mp_obj_t ip_address, mp_float_t timeout) {
    (void)self;
    mp_buffer_info_t packed;
    mp_get_buffer_raise(common_hal_ipaddress_ipv4address_get_packed(MP_OBJ_TO_PTR(ip_address)), &packed, MP_BUFFER_READ);
    uint32_t address;
    memcpy(&address, packed.buf, 4);
    return host_ping_ms(address, (int)(timeout * 1000));
}
