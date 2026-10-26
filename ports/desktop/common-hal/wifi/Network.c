// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>

#include "py/enum.h"
#include "py/runtime.h"
#include "shared-bindings/wifi/AuthMode.h"
#include "shared-bindings/wifi/Network.h"
#include "shared-bindings/wifi/__init__.h"

#include "host_net.h"

mp_obj_t wifi_network_current(bool for_scan) {
    wifi_radio_obj_t *radio = &common_hal_wifi_radio_obj;
    if (!for_scan && !radio->connected) {
        return mp_const_none;
    }
    wifi_network_obj_t *self = mp_obj_malloc(wifi_network_obj_t, &wifi_network_type);
    host_wifi_t wifi;
    if (host_wifi_current(&wifi)) {
        memcpy(self->ssid, wifi.ssid, sizeof(self->ssid));
        memcpy(self->bssid, wifi.bssid, sizeof(self->bssid));
        self->rssi = (int8_t)wifi.rssi;
        self->channel = (uint8_t)wifi.channel;
        self->secured = true;
        return MP_OBJ_FROM_PTR(self);
    }
    // On Ethernet: the network the program asked for, a good signal on channel 6.
    const char *ssid = radio->connected ? radio->ssid : "CircuitPython host";
    snprintf(self->ssid, sizeof(self->ssid), "%.32s", ssid);
    static const uint8_t bssid[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };    // locally administered
    memcpy(self->bssid, bssid, sizeof(bssid));
    self->rssi = -50;
    self->channel = 6;
    self->secured = radio->connected ? radio->secured : true;
    return MP_OBJ_FROM_PTR(self);
}

mp_obj_t common_hal_wifi_network_get_ssid(wifi_network_obj_t *self) {
    return mp_obj_new_str(self->ssid, strlen(self->ssid));
}

mp_obj_t common_hal_wifi_network_get_bssid(wifi_network_obj_t *self) {
    return mp_obj_new_bytes(self->bssid, sizeof(self->bssid));
}

mp_obj_t common_hal_wifi_network_get_rssi(wifi_network_obj_t *self) {
    return MP_OBJ_NEW_SMALL_INT(self->rssi);
}

mp_obj_t common_hal_wifi_network_get_channel(wifi_network_obj_t *self) {
    return MP_OBJ_NEW_SMALL_INT(self->channel);
}

mp_obj_t common_hal_wifi_network_get_country(wifi_network_obj_t *self) {
    (void)self;
    return MP_OBJ_NEW_QSTR(MP_QSTR_);
}

mp_obj_t common_hal_wifi_network_get_authmode(wifi_network_obj_t *self) {
    mp_obj_t modes = mp_obj_new_list(0, NULL);
    if (self->secured) {
        mp_obj_list_append(modes, cp_enum_find(&wifi_authmode_type, AUTHMODE_WPA2));
        mp_obj_list_append(modes, cp_enum_find(&wifi_authmode_type, AUTHMODE_PSK));
    } else {
        mp_obj_list_append(modes, cp_enum_find(&wifi_authmode_type, AUTHMODE_OPEN));
    }
    return modes;
}
