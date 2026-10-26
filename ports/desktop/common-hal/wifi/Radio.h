// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "shared-bindings/wifi/PowerManagement.h"
#include "shared-bindings/wifi/ScannedNetworks.h"

// wifi.radio on a host: the computer's network. connect() succeeds at once (the computer is
// online already) and remembers the network named; the addresses are the computer's.
typedef struct {
    mp_obj_base_t base;
    bool enabled;
    bool connected;
    char ssid[33];              // what connect() named
    bool secured;               // connect() had a password
    char hostname[254];         // "" = the computer's
    uint8_t mac[6];
    bool mac_set;               // set_mac_address() overrides the computer's
    mp_float_t tx_power;
    wifi_power_management_t power_management;
    uint32_t dns;               // ipv4_dns set by the program, 0 = the computer's
    wifi_scannednetworks_obj_t *current_scan;
} wifi_radio_obj_t;
