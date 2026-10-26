// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// A network a scan found or ap_info reports: the computer's Wi-Fi, or one standing in for it.
typedef struct {
    mp_obj_base_t base;
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    bool secured;
} wifi_network_obj_t;

// The network the radio is on, or (when scanning, not connected) the computer's.
mp_obj_t wifi_network_current(bool for_scan);
