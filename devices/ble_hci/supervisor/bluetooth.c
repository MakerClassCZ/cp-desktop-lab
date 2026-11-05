// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// The BLE workflow (supervisor/shared/bluetooth/) needs nothing here: the HCI background
// (bleio_hci_background()) runs supervisor_bluetooth_background(), and its attributes stay off
// the VM heap (Adapter.h).
