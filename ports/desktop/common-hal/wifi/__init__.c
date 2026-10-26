// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/gc.h"
#include "shared-bindings/wifi/Radio.h"
#include "shared-bindings/wifi/__init__.h"

#include "host_net.h"

wifi_radio_obj_t common_hal_wifi_radio_obj = {
    .base = { &wifi_radio_type },
    .enabled = true,
    .tx_power = (mp_float_t)20,
    .power_management = POWER_MANAGEMENT_NONE,
};

void common_hal_wifi_init(bool user_initiated) {
    (void)user_initiated;
    common_hal_wifi_radio_obj.base.type = &wifi_radio_type;
}

// After a program: a scan ends; the computer stays online, so the connection does too.
void wifi_user_reset(void) {
    common_hal_wifi_radio_obj.current_scan = NULL;
}

void common_hal_wifi_gc_collect(void) {
    gc_collect_ptr(common_hal_wifi_radio_obj.current_scan);
}

const char *wifi_get_hostname_raw(void) {
    wifi_radio_obj_t *self = &common_hal_wifi_radio_obj;
    if (self->hostname[0] == '\0') {
        host_hostname(self->hostname, sizeof(self->hostname));
    }
    return self->hostname;
}
