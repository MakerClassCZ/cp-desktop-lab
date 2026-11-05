// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2020 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2016 Glenn Ruben Bakke
// SPDX-FileCopyrightText: Copyright (c) 2018 Artur Pacholec
//
// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "att.h"
#include "bonding.h"
#include "hci.h"
#include "smp.h"

#include "py/gc.h"
#include "shared/runtime/interrupt_char.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "supervisor/shared/safe_mode.h"
#include "supervisor/shared/tick.h"
#include "supervisor/usb.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Adapter.h"
#include "shared-bindings/_bleio/Address.h"
#include "shared-bindings/_bleio/Characteristic.h"
#include "shared-bindings/_bleio/Service.h"
#include "shared-bindings/_bleio/Connection.h"
#include "shared-bindings/_bleio/ScanEntry.h"
#include "shared-bindings/time/__init__.h"

#define MSEC_TO_UNITS(TIME, RESOLUTION) (((TIME) * 1000) / (RESOLUTION))
#define SEC_TO_UNITS(TIME, RESOLUTION) (((TIME) * 1000000) / (RESOLUTION))
#define UNITS_TO_SEC(TIME, RESOLUTION) (((TIME)*(RESOLUTION)) / 1000000)
// 0.625 msecs (625 usecs)
#define ADV_INTERVAL_UNIT_FLOAT_SECS (0.000625)
// Microseconds is the base unit. The macros above know that.
#define UNIT_0_625_MS (625)
#define UNIT_1_25_MS  (1250)
#define UNIT_10_MS    (10000)

#define MAX_ADVERTISEMENT_SIZE (31)

// TODO make this settable from Python.
#define DEFAULT_TX_POWER 0  // 0 dBm
#define MAX_ANONYMOUS_ADV_TIMEOUT_SECS (60 * 15)
#define MAX_LIMITED_DISCOVERABLE_ADV_TIMEOUT_SECS (180)

#define BLE_MIN_CONN_INTERVAL        MSEC_TO_UNITS(15, UNIT_0_625_MS)
#define BLE_MAX_CONN_INTERVAL        MSEC_TO_UNITS(15, UNIT_0_625_MS)
#define BLE_SLAVE_LATENCY            0
#define BLE_CONN_SUP_TIMEOUT         MSEC_TO_UNITS(4000, UNIT_10_MS)

bleio_connection_internal_t bleio_connections[BLEIO_TOTAL_CONNECTION_COUNT];

static void add_generic_services(bleio_adapter_obj_t *adapter) {
    // Create Generic Access UUID, Service, and Characteristics.

    // Generic Access Service setup.

    bleio_uuid_obj_t *generic_access_service_uuid = mp_obj_malloc(bleio_uuid_obj_t, &bleio_uuid_type);
    common_hal_bleio_uuid_construct(generic_access_service_uuid, 0x1800, NULL);

    bleio_uuid_obj_t *device_name_characteristic_uuid = mp_obj_malloc(bleio_uuid_obj_t, &bleio_uuid_type);
    common_hal_bleio_uuid_construct(device_name_characteristic_uuid, 0x2A00, NULL);

    bleio_uuid_obj_t *appearance_characteristic_uuid = mp_obj_malloc(bleio_uuid_obj_t, &bleio_uuid_type);
    common_hal_bleio_uuid_construct(appearance_characteristic_uuid, 0x2A01, NULL);

    // Not implemented:
    // Peripheral Preferred Connection Parameters
    // Central Address Resolution

    bleio_service_obj_t *generic_access_service = mp_obj_malloc(bleio_service_obj_t, &bleio_service_type);
    common_hal_bleio_service_construct(generic_access_service, generic_access_service_uuid, false);

    adapter->device_name_characteristic = mp_obj_malloc(bleio_characteristic_obj_t, &bleio_characteristic_type);

    char generic_name[] = { 'C', 'I', 'R', 'C', 'U', 'I', 'T', 'P', 'Y', 'n', 'n', 'n', 'n' };
    mp_buffer_info_t generic_name_bufinfo = {
        .buf = generic_name,
        .len = sizeof(generic_name),
    };

    // Will be added to service by constructor.
    common_hal_bleio_characteristic_construct(
        adapter->device_name_characteristic,
        generic_access_service,
        BLE_GATT_HANDLE_INVALID,
        device_name_characteristic_uuid,
        CHAR_PROP_READ,
        SECURITY_MODE_OPEN,
        SECURITY_MODE_NO_ACCESS,
        248,                    // max length, from Bluetooth spec
        false,                  // not fixed length
        &generic_name_bufinfo,
        NULL
        );

    uint16_t zero_16 = 0;
    mp_buffer_info_t zero_16_value = {
        .buf = &zero_16,
        .len = sizeof(zero_16),
    };

    adapter->appearance_characteristic = mp_obj_malloc(bleio_characteristic_obj_t, &bleio_characteristic_type);

    common_hal_bleio_characteristic_construct(
        adapter->appearance_characteristic,
        generic_access_service,
        BLE_GATT_HANDLE_INVALID,
        appearance_characteristic_uuid,
        CHAR_PROP_READ,
        SECURITY_MODE_OPEN,
        SECURITY_MODE_NO_ACCESS,
        2,                      // max length, from Bluetooth spec
        true,                  // fixed length
        &zero_16_value,
        NULL
        );

    // Generic Attribute Service setup.

    bleio_uuid_obj_t *generic_attribute_service_uuid = mp_obj_malloc(bleio_uuid_obj_t, &bleio_uuid_type);
    common_hal_bleio_uuid_construct(generic_attribute_service_uuid, 0x1801, NULL);

    bleio_uuid_obj_t *service_changed_characteristic_uuid = mp_obj_malloc(bleio_uuid_obj_t, &bleio_uuid_type);
    common_hal_bleio_uuid_construct(service_changed_characteristic_uuid, 0x2A05, NULL);

    bleio_service_obj_t *generic_attribute_service = mp_obj_malloc(bleio_service_obj_t, &bleio_service_type);
    common_hal_bleio_service_construct(generic_attribute_service, generic_attribute_service_uuid, false);

    adapter->service_changed_characteristic = mp_obj_malloc(bleio_characteristic_obj_t, &bleio_characteristic_type);

    uint32_t zero_32 = 0;
    mp_buffer_info_t zero_32_value = {
        .buf = &zero_32,
        .len = sizeof(zero_32),
    };

    common_hal_bleio_characteristic_construct(
        adapter->service_changed_characteristic,
        generic_attribute_service,
        BLE_GATT_HANDLE_INVALID,
        service_changed_characteristic_uuid,
        CHAR_PROP_INDICATE,
        SECURITY_MODE_OPEN,
        SECURITY_MODE_NO_ACCESS,
        4,                     // max length, from Bluetooth spec
        true,                  // fixed length
        &zero_32_value,
        NULL
        );
}


static void check_enabled(bleio_adapter_obj_t *adapter) {
    if (!common_hal_bleio_adapter_get_enabled(adapter)) {
        // A port's own adapter comes on at its first use, as a board's built-in one is always on:
        // adafruit_ble's BLERadio() makes services without enabling it.
        if (adapter->persistent) {
            common_hal_bleio_adapter_set_enabled(adapter, true);
            return;
        }
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Adapter not enabled"));
    }
}

// A controller that supports extended advertising refuses legacy advertising
// commands (Command Disallowed) once any extended command has been sent, so
// use the extended command set exclusively on such controllers.
static bool adapter_uses_extended_advertising(bleio_adapter_obj_t *adapter) {
    return BT_FEAT_LE_EXT_ADV(adapter->features);
}

// static bool adapter_on_ble_evt(ble_evt_t *ble_evt, void *self_in) {
//     bleio_adapter_obj_t *self = (bleio_adapter_obj_t*)self_in;

//     // For debugging.
//     // mp_printf(&mp_plat_print, "Adapter event: 0x%04x\n", ble_evt->header.evt_id);

//     switch (ble_evt->header.evt_id) {
//         case BLE_GAP_EVT_CONNECTED: {
//             // Find an empty connection. One must always be available because the SD has the same
//             // total connection limit.
//             bleio_connection_internal_t *connection;
//             for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
//                 connection = &bleio_connections[i];
//                 if (connection->conn_handle == BLE_CONN_HANDLE_INVALID) {
//                     break;
//                 }
//             }

//             // Central has connected.
//             ble_gap_evt_connected_t* connected = &ble_evt->evt.gap_evt.params.connected;

//             connection->conn_handle = ble_evt->evt.gap_evt.conn_handle;
//             connection->connection_obj = mp_const_none;
//             connection->pair_status = PAIR_NOT_PAIRED;
//             connection->mtu = 0;

//             ble_drv_add_event_handler_entry(&connection->handler_entry, connection_on_ble_evt, connection);
//             self->connection_objs = NULL;

//             // Save the current connection parameters.
//             memcpy(&connection->conn_params, &connected->conn_params, sizeof(ble_gap_conn_params_t));

//             #if CIRCUITPY_VERBOSE_BLE
//             ble_gap_conn_params_t *cp = &connected->conn_params;
//             mp_printf(&mp_plat_print, "conn params: min_ci %d max_ci %d s_l %d sup_timeout %d\n", cp->min_conn_interval, cp->max_conn_interval, cp->slave_latency, cp->conn_sup_timeout);
//             #endif

//             // See if connection interval set by Central is out of range.
//             // If so, negotiate our preferred range.
//             ble_gap_conn_params_t conn_params;
//             sd_ble_gap_ppcp_get(&conn_params);
//             if (conn_params.min_conn_interval < connected->conn_params.min_conn_interval ||
//                 conn_params.min_conn_interval > connected->conn_params.max_conn_interval) {
//                 sd_ble_gap_conn_param_update(ble_evt->evt.gap_evt.conn_handle, &conn_params);
//             }
//             self->current_advertising_data = NULL;
//             break;
//         }
//         case BLE_GAP_EVT_DISCONNECTED: {
//             // Find the connection that was disconnected.
//             bleio_connection_internal_t *connection;
//             for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
//                 connection = &bleio_connections[i];
//                 if (connection->conn_handle == ble_evt->evt.gap_evt.conn_handle) {
//                     break;
//                 }
//             }
//             ble_drv_remove_event_handler(connection_on_ble_evt, connection);
//             connection->conn_handle = BLE_CONN_HANDLE_INVALID;
//             connection->pair_status = PAIR_NOT_PAIRED;
//             if (connection->connection_obj != mp_const_none) {
//                 bleio_connection_obj_t* obj = connection->connection_obj;
//                 obj->connection = NULL;
//                 obj->disconnect_reason = ble_evt->evt.gap_evt.params.disconnected.reason;
//             }
//             self->connection_objs = NULL;

//             break;
//         }

//         case BLE_GAP_EVT_ADV_SET_TERMINATED:
//             self->current_advertising_data = NULL;
//             break;

//         default:
//             // For debugging.
//             // mp_printf(&mp_plat_print, "Unhandled adapter event: 0x%04x\n", ble_evt->header.evt_id);
//             return false;
//             break;
//     }
//     return true;
// }

static void _adapter_set_name(bleio_adapter_obj_t *self, mp_obj_str_t *name_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(name_obj, &bufinfo, MP_BUFFER_READ);
    bleio_characteristic_set_local_value(self->device_name_characteristic, &bufinfo);
}

// Get various values and limits set by the adapter.
// Set event mask.
static void bleio_adapter_hci_init(bleio_adapter_obj_t *self) {
    // Get version information.
    if (hci_read_local_version(&self->hci_version, &self->hci_revision, &self->lmp_version,
        &self->manufacturer, &self->lmp_subversion) != HCI_OK) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Could not read HCI version"));
    }
    // Get supported features.
    if (hci_le_read_local_supported_features(self->features) != HCI_OK) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Could not read BLE features"));
    }

    // A controller without a public address (00:00:00:00:00:00, as Zephyr's on an nRF) is used
    // with a random static one: the controller's own if it has one, else one from LE Rand.
    self->own_addr_type = BT_ADDR_LE_PUBLIC;
    bt_addr_t public_address;
    static const bt_addr_t no_address = { 0 };
    const bool have_public_address = hci_read_bd_addr(&public_address) == HCI_OK;
    self->identity.type = BT_ADDR_LE_PUBLIC;
    self->identity.a = public_address;
    if (have_public_address &&
        memcmp(public_address.val, no_address.val, sizeof(no_address.val)) == 0) {
        if (hci_vs_read_static_address(&self->random_address) != HCI_OK) {
            // Made once and kept in the bond store: a new one at every enable would be a new
            // device to a phone each time, and its bond of no use.
            bonding_store_t *store = bonding_store();
            if (store->header.flags & BONDING_HAVE_STATIC_ADDRESS) {
                self->random_address = store->header.static_address;
            } else {
                uint8_t rand[8];
                hci_check_error(hci_le_rand(rand));
                memcpy(self->random_address.val, rand, sizeof(self->random_address.val));
                self->random_address.val[5] |= 0xc0;
                store->header.static_address = self->random_address;
                store->header.flags |= BONDING_HAVE_STATIC_ADDRESS;
                bonding_save();
            }
        }
        // A static address has its two top bits set.
        self->random_address.val[5] |= 0xc0;
        hci_check_error(hci_le_set_random_address(self->random_address.val));
        self->own_addr_type = BT_ADDR_LE_RANDOM;
        self->identity.type = BT_ADDR_LE_RANDOM;
        self->identity.a = self->random_address;
    }
    self->identity_known = have_public_address || self->own_addr_type == BT_ADDR_LE_RANDOM;

    // Enabled desired events.
    // Most importantly, includes:
    // BT_EVT_MASK_LE_META_EVENT                BT_EVT_BIT(61)
    if (hci_set_event_mask(0x3FFFFFFFFFFFFFFF) != HCI_OK) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Could not set event mask"));
    }
    // The default LE events (connection complete, advertising report, connection update, remote
    // features, LTK request) and the two LE Secure Connections will want: the local P-256 public
    // key and the DHKey. A controller that refuses keeps its default, which is enough, except
    // with extended advertising: it is scanned and connected with the extended commands, whose
    // events (LE Enhanced Connection Complete, LE Extended Advertising Report) it sends only when
    // they are unmasked.
    const bool extended = adapter_uses_extended_advertising(self);
    uint64_t le_events = BT_EVT_MASK_LE_CONN_COMPLETE | BT_EVT_MASK_LE_ADVERTISING_REPORT |
        BT_EVT_MASK_LE_CONN_UPDATE_COMPLETE | BT_EVT_MASK_LE_REMOTE_FEAT_COMPLETE |
        BT_EVT_MASK_LE_LTK_REQUEST | BT_EVT_MASK_LE_P256_PUBLIC_KEY_COMPLETE |
        BT_EVT_MASK_LE_GENERATE_DHKEY_COMPLETE;
    if (extended) {
        le_events |= BT_EVT_MASK_LE_ENH_CONN_COMPLETE | BT_EVT_MASK_LE_EXT_ADVERTISING_REPORT;
    }
    if (hci_le_set_event_mask(le_events) != HCI_OK && extended) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Could not set event mask"));
    }
    // Which commands the controller has: Secure Connections will ask it for the P-256 ones.
    if (hci_read_local_supported_commands(self->supported_commands) != HCI_OK) {
        memset(self->supported_commands, 0, sizeof(self->supported_commands));
    }

    // Get ACL buffer info.
    uint16_t le_max_len;
    uint8_t le_max_num;
    if (hci_le_read_buffer_size(&le_max_len, &le_max_num) == HCI_OK) {
        self->max_acl_buffer_len = le_max_len;
        self->max_acl_num_buffers = le_max_num;
    } else {
        // LE Read Buffer Size not available; use the general Read Buffer Size.
        uint16_t acl_max_len;
        uint8_t sco_max_len;
        uint16_t acl_max_num;
        uint16_t sco_max_num;
        if (hci_read_buffer_size(&acl_max_len, &sco_max_len, &acl_max_num, &sco_max_num) != HCI_OK) {
            mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Could not read BLE buffer info"));
        }
        self->max_acl_buffer_len = acl_max_len;
        self->max_acl_num_buffers = acl_max_num;
    }

    // The default name ends with the address, so it comes last; the generic services hold it.
    self->name = NULL;
    bleio_adapter_reset_name(self);
}

void common_hal_bleio_adapter_construct_hci_transport(bleio_adapter_obj_t *self,
    const bleio_hci_transport_t *transport, void *ctx) {
    self->allocated = true;
    self->hci_transport = transport;
    self->hci_transport_ctx = ctx;

    // Advertising-related fields are initialized by common_hal_bleio_adapter_set_enabled().
    self->enabled = false;

    common_hal_bleio_adapter_set_enabled(self, true);
}

void common_hal_bleio_adapter_construct_hci_uart(bleio_adapter_obj_t *self, busio_uart_obj_t *uart, digitalio_digitalinout_obj_t *rts, digitalio_digitalinout_obj_t *cts) {
    self->hci_uart = uart;
    self->rts_digitalinout = rts;
    self->cts_digitalinout = cts;
    common_hal_bleio_adapter_construct_hci_transport(self, &bleio_hci_uart_transport, self);
}

void common_hal_bleio_adapter_set_enabled(bleio_adapter_obj_t *self, bool enabled) {
    const bool is_enabled = common_hal_bleio_adapter_get_enabled(self);

    // Don't enable or disable twice
    if (is_enabled == enabled) {
        return;
    }

    // We must poll for input from the HCI adapter.
    // TODO Can we instead trigger an interrupt on UART input traffic?
    if (enabled) {
        self->enabled = true;
        supervisor_enable_tick();
        if (self->hci_transport->enabled != NULL) {
            self->hci_transport->enabled(self->hci_transport_ctx, true);
        }
    }

    // Enabling or disabling: stop any current activity; reset to known state. While disabling
    // the adapter still counts as enabled here, so the background poll reads the reset's reply:
    // left unread, the next enable takes it for the answer to its own reset and talks to a
    // controller still resetting.
    hci_reset();
    if (!enabled) {
        self->enabled = false;
        supervisor_disable_tick();
        if (self->hci_transport->enabled != NULL) {
            self->hci_transport->enabled(self->hci_transport_ctx, false);
        }
    }
    self->now_advertising = false;
    self->circuitpython_advertising = false;
    self->advertising_timeout_msecs = 0;
    // The reset stopped any scan too.
    if (self->scan_results != NULL) {
        shared_module_bleio_scanresults_set_done(self->scan_results, true);
        self->scan_results = NULL;
    }
    self->scan_timeout_msecs = 0;

    if (enabled) {
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            // Reset list of known attributes.
            // Indices into the list are handles. Handle 0x0000 designates an invalid handle,
            // so store None there to skip it.
            self->attributes = mp_obj_new_list(0, NULL);
            bleio_adapter_add_attribute(self, mp_const_none);
            add_generic_services(self);

            // The reset cleared the event mask and the random address: set the controller up again.
            bleio_adapter_hci_init(self);
            nlr_pop();
        } else {
            // A controller that does not answer leaves the adapter off, not on with no buffers.
            self->enabled = false;
            supervisor_disable_tick();
            if (self->hci_transport->enabled != NULL) {
                self->hci_transport->enabled(self->hci_transport_ctx, false);
            }
            nlr_jump(nlr.ret_val);
        }
    }
}

bool common_hal_bleio_adapter_get_enabled(bleio_adapter_obj_t *self) {
    return self->enabled;
}

bleio_address_obj_t *common_hal_bleio_adapter_get_address(bleio_adapter_obj_t *self) {
    check_enabled(self);

    bt_addr_t addr;
    if (self->own_addr_type == BT_ADDR_LE_RANDOM) {
        addr = self->random_address;
    } else {
        hci_check_error(hci_read_bd_addr(&addr));
    }

    // The cached address is refreshed on every call.
    common_hal_bleio_address_construct(&self->address, addr.val, self->own_addr_type);
    self->address.base.type = &bleio_address_type;
    return &self->address;
}

bool common_hal_bleio_adapter_set_address(bleio_adapter_obj_t *self, bleio_address_obj_t *address) {
    if (hci_le_set_random_address(address->bytes) != HCI_OK) {
        return false;
    }
    // Advertising uses it from now on, as a random address.
    memcpy(self->random_address.val, address->bytes, sizeof(self->random_address.val));
    self->own_addr_type = BT_ADDR_LE_RANDOM;
    return true;
}

mp_obj_str_t *common_hal_bleio_adapter_get_name(bleio_adapter_obj_t *self) {
    if (self->name == NULL) {
        // A persistent adapter not used in this run yet: on, its name set; any other raises.
        check_enabled(self);
    }
    return self->name;
}

void common_hal_bleio_adapter_set_name(bleio_adapter_obj_t *self, const char *name) {
    // The name lives in the GAP service, which enabling makes: a persistent adapter comes on.
    check_enabled(self);
    self->name = mp_obj_new_str(name, strlen(name));
    _adapter_set_name(self, self->name);
}


// The controller's own random address set again before it is used (scanning actively,
// connecting): an anonymous advertisement may have left another one there. Not while advertising,
// when a controller refuses it (Command Disallowed).
static void set_own_random_address(bleio_adapter_obj_t *self) {
    if (self->own_addr_type == BT_ADDR_LE_RANDOM && !self->now_advertising) {
        hci_check_error(hci_le_set_random_address(self->random_address.val));
    }
}

static bool scanning(bleio_adapter_obj_t *self) {
    return self->scan_results != NULL && !shared_module_bleio_scanresults_get_done(self->scan_results);
}

mp_obj_t common_hal_bleio_adapter_start_scan(bleio_adapter_obj_t *self, uint8_t *prefixes, size_t prefix_length, bool extended, mp_int_t buffer_size, mp_float_t timeout, mp_float_t interval, mp_float_t window, mp_int_t minimum_rssi, bool active) {
    check_enabled(self);

    if (scanning(self)) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Scan already in progress. Stop with stop_scan."));
    }
    self->scan_results = shared_module_bleio_new_scanresults(buffer_size, prefixes, prefix_length, minimum_rssi);
    // The controller reports what it hears; the results take it from the event (hci.c).
    const uint16_t interval_units = SEC_TO_UNITS(interval, UNIT_0_625_MS);
    const uint16_t window_units = SEC_TO_UNITS(window, UNIT_0_625_MS);
    const uint8_t scan_type = active ? BT_HCI_LE_SCAN_ACTIVE : BT_HCI_LE_SCAN_PASSIVE;
    if (active) {
        set_own_random_address(self);
    }
    int result;
    if (adapter_uses_extended_advertising(self)) {
        // The extended commands only, on a controller that has them (as for advertising). The
        // `extended` argument (longer advertisements) is what the reports may carry either way.
        result = hci_le_set_extended_scan_parameters(self->own_addr_type, scan_type, interval_units, window_units);
        if (result == HCI_OK) {
            result = hci_le_set_extended_scan_enable(BT_HCI_LE_SCAN_ENABLE, BT_HCI_LE_SCAN_FILTER_DUP_DISABLE);
        }
    } else {
        result = hci_le_set_scan_parameters(scan_type, interval_units, window_units, self->own_addr_type, 0x00);
        if (result == HCI_OK) {
            // Every report, duplicates too, as the nRF's SoftDevice gives them: adafruit_ble
            // tracks what it has seen itself.
            result = hci_le_set_scan_enable(BT_HCI_LE_SCAN_ENABLE, BT_HCI_LE_SCAN_FILTER_DUP_DISABLE);
        }
    }
    if (result != HCI_OK) {
        shared_module_bleio_scanresults_set_done(self->scan_results, true);
        self->scan_results = NULL;
        hci_check_error(result);
    }
    // Ended by bleio_adapter_background() after timeout seconds; 0: scans until stop_scan().
    self->scan_timeout_msecs = (uint64_t)(timeout * 1000);
    self->scan_start_ticks = supervisor_ticks_ms64();
    vm_used_ble = true;
    return MP_OBJ_FROM_PTR(self->scan_results);
}

void common_hal_bleio_adapter_stop_scan(bleio_adapter_obj_t *self) {
    check_enabled(self);
    if (!scanning(self)) {
        return;
    }
    shared_module_bleio_scanresults_set_done(self->scan_results, true);
    self->scan_results = NULL;
    self->scan_timeout_msecs = 0;
    if (adapter_uses_extended_advertising(self)) {
        hci_check_error(hci_le_set_extended_scan_enable(BT_HCI_LE_SCAN_DISABLE, BT_HCI_LE_SCAN_FILTER_DUP_DISABLE));
    } else {
        hci_check_error(hci_le_set_scan_enable(BT_HCI_LE_SCAN_DISABLE, BT_HCI_LE_SCAN_FILTER_DUP_DISABLE));
    }
}

void bleio_adapter_scan_report(bleio_adapter_obj_t *self, bool connectable, bool scan_response,
    int8_t rssi, const bt_addr_le_t *addr, const uint8_t *data, uint8_t len) {
    if (!scanning(self)) {
        return;
    }
    // A random address's two top bits tell its kind: 11 static, 01 resolvable, 00 not.
    uint8_t type = BLEIO_ADDRESS_TYPE_PUBLIC;
    if (addr->type == BT_ADDR_LE_RANDOM) {
        const uint8_t top = addr->a.val[5] >> 6;
        type = top == 0x3 ? BLEIO_ADDRESS_TYPE_RANDOM_STATIC
            : top == 0x1 ? BLEIO_ADDRESS_TYPE_RANDOM_PRIVATE_RESOLVABLE
            : BLEIO_ADDRESS_TYPE_RANDOM_PRIVATE_NON_RESOLVABLE;
    } else if (addr->type == BT_ADDR_LE_RANDOM_ID) {
        type = BLEIO_ADDRESS_TYPE_RANDOM_STATIC;
    }
    shared_module_bleio_scanresults_append(self->scan_results, supervisor_ticks_ms64(), connectable,
        scan_response, rssi, addr->a.val, type, data, len);
}

mp_obj_t common_hal_bleio_adapter_connect(bleio_adapter_obj_t *self, bleio_address_obj_t *address, mp_float_t timeout) {
    check_enabled(self);

    // A controller that scans may refuse to initiate; the scan has done its work.
    common_hal_bleio_adapter_stop_scan(self);
    set_own_random_address(self);

    bt_addr_le_t peer = {
        .type = address->type == BLEIO_ADDRESS_TYPE_PUBLIC ? BT_ADDR_LE_PUBLIC : BT_ADDR_LE_RANDOM,
    };
    memcpy(peer.a.val, address->bytes, sizeof(peer.a.val));

    vm_used_ble = true;
    self->connect_status = -1;
    // Scan at 60 ms in 30 ms windows; a connection interval of 7.5 to 15 ms, no latency, a 2 s
    // supervision timeout, as ArduinoBLE asks.
    if (adapter_uses_extended_advertising(self)) {
        hci_check_error(hci_le_extended_create_conn(0x00, self->own_addr_type, &peer, 0x0060, 0x0030,
            0x0006, 0x000c, 0x0000, 0x00c8, 0x0004, 0x0006));
    } else {
        hci_check_error(hci_le_create_conn(0x0060, 0x0030, 0x00, &peer, self->own_addr_type,
            0x0006, 0x000c, 0x0000, 0x00c8, 0x0004, 0x0006));
    }

    // LE Connection Complete sets connect_status: 0 connected, else the controller's error.
    const uint64_t start = supervisor_ticks_ms64();
    const uint64_t timeout_ms = (uint64_t)(timeout * 1000);
    while (self->connect_status < 0 && !mp_hal_is_interrupted() &&
           (timeout_ms == 0 || supervisor_ticks_ms64() - start < timeout_ms)) {
        RUN_BACKGROUND_TASKS;
    }
    if (self->connect_status < 0) {
        // Not in time (or Ctrl-C): the controller stops trying, and says so with a Connection
        // Complete of status Unknown Connection Identifier, or of a connection that came first.
        if (hci_le_cancel_conn() == HCI_OK) {
            const uint64_t cancelled = supervisor_ticks_ms64();
            while (self->connect_status < 0 && supervisor_ticks_ms64() - cancelled < 1000) {
                RUN_BACKGROUND_TASKS;
            }
        }
    }
    if (mp_hal_is_interrupted()) {
        // Ctrl-C: the KeyboardInterrupt comes when this returns.
        return mp_const_none;
    }
    const uint16_t conn_handle = self->connect_status == 0 ? att_conn_handle(&peer) : BLE_CONN_HANDLE_INVALID;
    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        bleio_connection_internal_t *connection = &bleio_connections[i];
        if (conn_handle != BLE_CONN_HANDLE_INVALID && connection->conn_handle == conn_handle) {
            return bleio_connection_new_from_internal(connection);
        }
    }
    if (self->connect_status <= 0 || self->connect_status == BT_HCI_ERR_UNKNOWN_CONN_ID) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Failed to connect: timeout"));
    }
    mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("HCI status error: %02x"), self->connect_status);
    return mp_const_none;
}

static void check_data_fit(size_t data_len, bool connectable) {
    if (data_len > MAX_ADVERTISEMENT_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("Data too large for advertisement packet"));
    }
}

// static bool advertising_on_ble_evt(ble_evt_t *ble_evt, void *self_in) {
//     bleio_adapter_obj_t *self = (bleio_adapter_obj_t*)self_in;

//     switch (ble_evt->header.evt_id) {
//         case BLE_GAP_EVT_ADV_SET_TERMINATED:
//             common_hal_bleio_adapter_stop_advertising(self);
//             ble_drv_remove_event_handler(advertising_on_ble_evt, self_in);
//             break;

//         default:
//             // For debugging.
//             // mp_printf(&mp_plat_print, "Unhandled advertising event: 0x%04x\n", ble_evt->header.evt_id);
//             return false;
//             break;
//     }
//     return true;
// }

uint32_t _common_hal_bleio_adapter_start_advertising(bleio_adapter_obj_t *self,
    bool connectable, bool anonymous, uint32_t timeout, float interval,
    const uint8_t *advertising_data, uint16_t advertising_data_len,
    const uint8_t *scan_response_data, uint16_t scan_response_data_len,
    mp_int_t tx_power, const bleio_address_obj_t *directed_to) {
    check_enabled(self);

    if (self->now_advertising) {
        if (self->circuitpython_advertising) {
            common_hal_bleio_adapter_stop_advertising(self);
        } else {
            // User-requested advertising.
            // TODO allow multiple advertisements.
            // Already advertising. Can't advertise twice.
            return 1;
        }
    }

    // Peer address, for directed advertising
    bt_addr_le_t peer_addr = { 0 };

    // Copy peer address, if supplied.
    if (directed_to) {
        peer_addr.type = directed_to->type;
        memcpy(&peer_addr.a.val, directed_to->bytes, sizeof(peer_addr.a.val));
    }

    // The random address this advertisement goes out from, when it is one: anonymous, a new
    // non-resolvable private address (random, the two top bits 00); else the static one of a
    // controller without a public address, set again, since an anonymous advertisement before
    // may have left the controller another.
    const uint8_t own_addr_type = anonymous ? BT_ADDR_LE_RANDOM : self->own_addr_type;
    bt_addr_t random_address = self->random_address;
    if (anonymous) {
        uint8_t rand[8];
        hci_check_error(hci_le_rand(rand));
        memcpy(random_address.val, rand, sizeof(random_address.val));
        random_address.val[5] &= 0x3f;
    }

    // Ours on the link a central makes to this advertising (pairing's c1 takes it).
    self->advertising_address.type = own_addr_type;
    self->advertising_address.a = own_addr_type == BT_ADDR_LE_RANDOM ? random_address : self->identity.a;

    if (adapter_uses_extended_advertising(self)) {
        // Use legacy PDUs (ADV_IND etc.) so that 4.x centrals still see us and the
        // 31-byte data limit applies unchanged; only the HCI command set is extended.
        uint16_t props = BT_HCI_LE_ADV_PROP_LEGACY;
        if (connectable) {
            if (directed_to) {
                props |= BT_HCI_LE_ADV_PROP_CONN | BT_HCI_LE_ADV_PROP_DIRECT;
            } else {
                props |= BT_HCI_LE_ADV_PROP_CONN | BT_HCI_LE_ADV_PROP_SCAN;
            }
        } else if (scan_response_data_len > 0) {
            props |= BT_HCI_LE_ADV_PROP_SCAN;
        }

        // Advertising interval.
        uint32_t interval_units = SEC_TO_UNITS(interval, UNIT_0_625_MS);

        hci_check_error(
            hci_le_set_extended_advertising_parameters(
                0,              // handle
                props,          // adv properties
                interval_units, // min interval
                interval_units, // max interval
                0b111,          // channel map: channels 37, 38, 39
                own_addr_type,
                &peer_addr,
                0x00,           // filter policy: no filter
                BT_HCI_LE_ADV_TX_POWER_NO_PREF,
                BT_HCI_LE_EXT_SCAN_PHY_1M, // primary PHY
                0x00,                      // secondary max skip (unused with legacy PDUs)
                BT_HCI_LE_EXT_SCAN_PHY_1M, // secondary PHY (unused with legacy PDUs)
                0x00,                      // advertising SID
                0x00                       // scan request notification disabled
                ));

        // An advertising set has a random address of its own, set once the set exists.
        if (own_addr_type == BT_ADDR_LE_RANDOM) {
            hci_check_error(hci_le_set_advertising_set_random_address(0, random_address.val));
        }

        hci_check_error(
            hci_le_set_extended_advertising_data(
                0,                                  // handle
                BT_HCI_LE_EXT_ADV_OP_COMPLETE_DATA,
                BT_HCI_LE_EXT_ADV_FRAG_DISABLED,
                advertising_data_len,
                (uint8_t *)advertising_data));

        // A scannable set must have scan response data defined before it is enabled,
        // even if that data is empty.
        if (props & BT_HCI_LE_ADV_PROP_SCAN) {
            hci_check_error(
                hci_le_set_extended_scan_response_data(
                    0,                                  // handle
                    BT_HCI_LE_EXT_ADV_OP_COMPLETE_DATA,
                    BT_HCI_LE_EXT_ADV_FRAG_DISABLED,
                    scan_response_data_len,
                    (uint8_t *)scan_response_data));
        }

        // Use our own timeout rather than the controller's duration, so that we don't
        // depend on receiving the LE Advertising Set Terminated event.
        self->advertising_timeout_msecs = timeout * 1000;
        self->advertising_start_ticks = supervisor_ticks_ms64();

        // Duration 0 and max events 0 mean "advertise until disabled by the host".
        uint8_t handle[1] = { 0 };
        uint16_t duration_forever[1] = { 0 };
        uint8_t max_ext_adv_evts[1] = { 0 };
        hci_check_error(
            hci_le_set_extended_advertising_enable(
                BT_HCI_LE_ADV_ENABLE,
                1,                // one advertising set.
                handle,
                duration_forever,
                max_ext_adv_evts
                ));
    } else {
        // Legacy advertising (not extended).

        uint8_t adv_type;
        if (connectable) {
            // Connectable, scannable, undirected.
            adv_type = BT_HCI_ADV_IND;
        } else if (scan_response_data_len > 0) {
            // Unconnectable, scannable, undirected.
            adv_type = BT_HCI_ADV_SCAN_IND;
        } else {
            // Unconnectable, unscannable, undirected.
            adv_type = BT_HCI_ADV_NONCONN_IND;
        }

        // Advertising interval.
        uint16_t interval_units = SEC_TO_UNITS(interval, UNIT_0_625_MS);

        if (own_addr_type == BT_ADDR_LE_RANDOM) {
            hci_check_error(hci_le_set_random_address(random_address.val));
        }

        hci_check_error(
            hci_le_set_advertising_parameters(
                interval_units, // min interval
                interval_units, // max interval
                adv_type,
                own_addr_type,
                &peer_addr,
                0b111,          // channel map: channels 37, 38, 39
                0x00            // filter policy: no filter
                ));

        // The HCI commands expect MAX_ADVERTISEMENT_SIZE (31)octets,
        // even though the actual data length may be shorter.
        uint8_t full_data[MAX_ADVERTISEMENT_SIZE] = { 0 };
        memcpy(full_data, advertising_data, MIN(sizeof(full_data), advertising_data_len));
        hci_check_error(hci_le_set_advertising_data(advertising_data_len, full_data));
        memset(full_data, 0, sizeof(full_data));
        if (scan_response_data_len > 0) {
            memcpy(full_data, scan_response_data, MIN(sizeof(full_data), scan_response_data_len));
            hci_check_error(hci_le_set_scan_response_data(scan_response_data_len, full_data));
        }

        // No duration mechanism is provided for legacy advertising, so we need to do our own.
        self->advertising_timeout_msecs = timeout * 1000;
        self->advertising_start_ticks = supervisor_ticks_ms64();

        // Start advertising.
        hci_check_error(hci_le_set_advertising_enable(BT_HCI_LE_ADV_ENABLE));
    } // end legacy advertising setup

    vm_used_ble = true;
    self->now_advertising = true;
    return 0;
}

void common_hal_bleio_adapter_start_advertising(bleio_adapter_obj_t *self,
    bool connectable, bool anonymous, uint32_t timeout, mp_float_t interval,
    mp_buffer_info_t *advertising_data_bufinfo,
    mp_buffer_info_t *scan_response_data_bufinfo,
    mp_int_t tx_power, const bleio_address_obj_t *directed_to) {
    check_enabled(self);

    // interval value has already been validated.

    check_data_fit(advertising_data_bufinfo->len, connectable);
    check_data_fit(scan_response_data_bufinfo->len, connectable);

    if (advertising_data_bufinfo->len > MAX_ADVERTISEMENT_SIZE && scan_response_data_bufinfo->len > 0) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Extended advertisements with scan response not supported."));
    }

    // Anonymous mode requires a timeout so that we don't continue to broadcast
    // the same data while cycling the MAC address -- otherwise, what's the
    // point of randomizing the MAC address?
    if (timeout == 0 && anonymous) {
        timeout = MAX_ANONYMOUS_ADV_TIMEOUT_SECS;
    } else {
        if (timeout > MAX_LIMITED_DISCOVERABLE_ADV_TIMEOUT_SECS) {
            mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Timeout is too long: Maximum timeout length is %d seconds"),
                MAX_LIMITED_DISCOVERABLE_ADV_TIMEOUT_SECS);
        }
    }

    if (tx_power != 0) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("Only tx_power=0 supported"));
    }

    const uint32_t result = _common_hal_bleio_adapter_start_advertising(
        self, connectable, anonymous, timeout, interval,
        advertising_data_bufinfo->buf,
        advertising_data_bufinfo->len,
        scan_response_data_bufinfo->buf,
        scan_response_data_bufinfo->len,
        tx_power, directed_to);

    if (result) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Already advertising"));
    }
    self->circuitpython_advertising = false;
}

void common_hal_bleio_adapter_stop_advertising(bleio_adapter_obj_t *self) {
    check_enabled(self);

    // A connection stops advertising in the controller (bleio_adapter_advertising_was_stopped):
    // asking again gets no answer from an ESP32 and Command Disallowed from a CYW43.
    if (!self->now_advertising) {
        self->circuitpython_advertising = false;
        return;
    }
    self->now_advertising = false;
    self->circuitpython_advertising = false;

    int result;
    if (adapter_uses_extended_advertising(self)) {
        // Duration and max events are ignored when disabling.
        uint8_t handle[1] = { 0 };
        uint16_t duration_forever[1] = { 0 };
        uint8_t max_ext_adv_evts[1] = { 0 };
        result = hci_le_set_extended_advertising_enable(BT_HCI_LE_ADV_DISABLE, 1, handle, duration_forever, max_ext_adv_evts);
    } else {
        result = hci_le_set_advertising_enable(BT_HCI_LE_ADV_DISABLE);
    }
    // OK if we're already stopped. There seems to be an ESP32 HCI bug:
    // If advertising is already off, then LE_SET_ADV_ENABLE does not return a response.
    if (result != HCI_RESPONSE_TIMEOUT) {
        hci_check_error(result);
    }

    // TODO startup CircuitPython advertising again.
}

// Note that something stopped advertising, such as a connection happening.
// Don't ask the adapter to stop.
void bleio_adapter_advertising_was_stopped(bleio_adapter_obj_t *self) {
    self->now_advertising = false;
    self->circuitpython_advertising = false;
}

bool common_hal_bleio_adapter_get_advertising(bleio_adapter_obj_t *self) {
    check_enabled(self);

    return self->now_advertising;
}

bool common_hal_bleio_adapter_get_connected(bleio_adapter_obj_t *self) {
    check_enabled(self);

    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        bleio_connection_internal_t *connection = &bleio_connections[i];
        if (connection->conn_handle != BLE_CONN_HANDLE_INVALID) {
            return true;
        }
    }
    return false;
}

mp_obj_t common_hal_bleio_adapter_get_connections(bleio_adapter_obj_t *self) {
    check_enabled(self);

    if (self->connection_objs != NULL) {
        return self->connection_objs;
    }
    size_t total_connected = 0;
    mp_obj_t items[BLEIO_TOTAL_CONNECTION_COUNT];
    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        bleio_connection_internal_t *connection = &bleio_connections[i];
        if (connection->conn_handle != BLE_CONN_HANDLE_INVALID) {
            if (connection->connection_obj == mp_const_none) {
                connection->connection_obj = bleio_connection_new_from_internal(connection);
            }
            items[total_connected] = connection->connection_obj;
            total_connected++;
        }
    }
    self->connection_objs = mp_obj_new_tuple(total_connected, items);
    return self->connection_objs;
}

// The identity the bonds are filed under: the one the last enable read, NULL before any (the
// bond store then answers for every identity). Asks nothing of the controller, so neither
// function below needs the adapter enabled, the VM heap, or can raise: the supervisor calls them
// before the BLE workflow starts.
static const bt_addr_le_t *bond_identity(bleio_adapter_obj_t *self) {
    return self->identity_known ? &self->identity : NULL;
}

void common_hal_bleio_adapter_erase_bonding(bleio_adapter_obj_t *self) {
    bonding_erase(bond_identity(self));
}

bool common_hal_bleio_adapter_is_bonded_to_central(bleio_adapter_obj_t *self) {
    // Bonded to a central: a bond where we are the peripheral.
    return bonding_is_bonded(bond_identity(self), BONDING_ROLE_PERIPHERAL);
}

uint16_t bleio_adapter_add_attribute(bleio_adapter_obj_t *adapter, mp_obj_t *attribute) {
    check_enabled(adapter);

    // The handle is the index of this attribute in the attributes list.
    uint16_t handle = (uint16_t)adapter->attributes->len;
    mp_obj_list_append(adapter->attributes, attribute);

    if (mp_obj_is_type(attribute, &bleio_service_type)) {
        adapter->last_added_service_handle = handle;
    }
    if (mp_obj_is_type(attribute, &bleio_characteristic_type)) {
        adapter->last_added_characteristic_handle = handle;
    }

    return handle;
}

mp_obj_t *bleio_adapter_get_attribute(bleio_adapter_obj_t *adapter, uint16_t handle) {
    check_enabled(adapter);

    if (handle == 0 || handle >= adapter->attributes->len) {
        return mp_const_none;
    }
    return adapter->attributes->items[handle];
}

uint16_t bleio_adapter_max_attribute_handle(bleio_adapter_obj_t *adapter) {
    check_enabled(adapter);

    return adapter->attributes->len - 1;
}


void bleio_adapter_gc_collect(bleio_adapter_obj_t *adapter) {
    gc_collect_root((void **)adapter, sizeof(bleio_adapter_obj_t) / (sizeof(size_t)));
    gc_collect_root((void **)bleio_connections, sizeof(bleio_connections) / (sizeof(size_t)));
}

void bleio_adapter_reset(bleio_adapter_obj_t *adapter) {

    if (!common_hal_bleio_adapter_get_enabled(adapter)) {
        return;
    }

    // Adapter will be reset.
    common_hal_bleio_adapter_set_enabled(adapter, false);

    adapter->connection_objs = NULL;
    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        bleio_connection_internal_t *connection = &bleio_connections[i];
        // Disconnect all connections with Python state cleanly. Keep any supervisor-only connections.
        if (connection->connection_obj != mp_const_none &&
            connection->conn_handle != BLE_CONN_HANDLE_INVALID) {
            common_hal_bleio_connection_disconnect(connection);
        }
        connection->connection_obj = mp_const_none;
    }
}

void bleio_adapter_background(bleio_adapter_obj_t *adapter) {
    if (!common_hal_bleio_adapter_get_enabled(adapter)) {
        return;
    }

    // Timeouts send commands: not from inside another command's wait for its answer (it would be
    // taken for theirs). Their own answers send_command() reads itself, as background tasks do
    // not nest.
    if (!hci_busy()) {
        if (adapter->advertising_timeout_msecs > 0 &&
            supervisor_ticks_ms64() - adapter->advertising_start_ticks > adapter->advertising_timeout_msecs) {
            adapter->advertising_timeout_msecs = 0;
            common_hal_bleio_adapter_stop_advertising(adapter);
        }
        if (adapter->scan_timeout_msecs > 0 &&
            supervisor_ticks_ms64() - adapter->scan_start_ticks > adapter->scan_timeout_msecs) {
            adapter->scan_timeout_msecs = 0;
            common_hal_bleio_adapter_stop_scan(adapter);
        }
    }

    hci_result_t result = hci_poll_for_incoming_pkt();
    if (result != HCI_OK) {
        mp_printf(&mp_plat_print, "bad hci_poll_for_incoming_pkt() result in background: %d\n", result);
    }
    // What the packets left for the Security Manager: its answers and commands, sent from here.
    smp_background();
}
