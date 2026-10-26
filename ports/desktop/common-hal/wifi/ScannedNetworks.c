// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/wifi/ScannedNetworks.h"

mp_obj_t common_hal_wifi_scannednetworks_next(wifi_scannednetworks_obj_t *self) {
    mp_obj_t network = self->network;
    self->network = MP_OBJ_NULL;
    return network != MP_OBJ_NULL ? network : mp_const_none;
}
