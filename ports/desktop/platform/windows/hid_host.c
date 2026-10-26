// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "hid_host.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

static char _last_error[128];

const char *hid_host_last_error(void) {
    return _last_error;
}

static HANDLE _handle = INVALID_HANDLE_VALUE;
static HANDLE _event;

static void _win_error(const char *what) {
    snprintf(_last_error, sizeof(_last_error), "%s: error %lu", what, (unsigned long)GetLastError());
}

bool hid_host_open(uint16_t vid, uint16_t pid, const char *what, char *err, size_t err_len) {
    if (_handle != INVALID_HANDLE_VALUE) {
        return true;
    }
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsA(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        snprintf(err, err_len, "cannot list HID devices (error %lu)", (unsigned long)GetLastError());
        return false;
    }
    bool found = false;
    DWORD open_error = 0;
    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA interface_data = { .cbSize = sizeof(interface_data) };
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &guid, i, &interface_data)) {
            break;
        }
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailA(set, &interface_data, NULL, 0, &size, NULL);
        if (size == 0 || size > 4096) {
            continue;
        }
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A detail;
            char bytes[4096];
        } buffer;
        buffer.detail.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &interface_data, &buffer.detail, size, NULL, NULL)) {
            continue;
        }
        // Match on the path (it carries vid_xxxx&pid_xxxx): a device another program holds
        // exclusively cannot be asked for its attributes, but it is still there.
        char wanted[32];
        snprintf(wanted, sizeof(wanted), "vid_%04x&pid_%04x", vid, pid);
        char lower[4096];
        size_t n = strlen(buffer.detail.DevicePath);
        for (size_t k = 0; k <= n && k < sizeof(lower) - 1; k++) {
            char c = buffer.detail.DevicePath[k];
            lower[k] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        }
        lower[sizeof(lower) - 1] = 0;
        if (strstr(lower, wanted) == NULL) {
            continue;
        }
        found = true;
        _handle = CreateFileA(buffer.detail.DevicePath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (_handle != INVALID_HANDLE_VALUE) {
            break;
        }
        open_error = GetLastError();
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!found) {
        snprintf(err, err_len, "no %s found (VID %04x PID %04x)", what, vid, pid);
        return false;
    }
    if (_handle == INVALID_HANDLE_VALUE) {
        snprintf(err, err_len, "cannot open %s: error %lu%s", what, (unsigned long)open_error,
            open_error == ERROR_SHARING_VIOLATION || open_error == ERROR_ACCESS_DENIED ? " (another program has it)" : "");
        return false;
    }
    _event = CreateEventA(NULL, TRUE, FALSE, NULL);
    return true;
}

bool hid_host_present(uint16_t vid, uint16_t pid) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsA(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        return false;
    }
    char wanted[32];
    snprintf(wanted, sizeof(wanted), "vid_%04x&pid_%04x", vid, pid);
    bool found = false;
    for (DWORD i = 0; !found; i++) {
        SP_DEVICE_INTERFACE_DATA interface_data = { .cbSize = sizeof(interface_data) };
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &guid, i, &interface_data)) {
            break;
        }
        union {
            SP_DEVICE_INTERFACE_DETAIL_DATA_A detail;
            char bytes[4096];
        } buffer;
        buffer.detail.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &interface_data, &buffer.detail, sizeof(buffer), NULL, NULL)) {
            continue;
        }
        for (char *p = buffer.detail.DevicePath; *p != 0; p++) {
            if (*p >= 'A' && *p <= 'Z') {
                *p = (char)(*p + 32);
            }
        }
        found = strstr(buffer.detail.DevicePath, wanted) != NULL;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

void hid_host_close(void) {
    if (_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(_handle);
        _handle = INVALID_HANDLE_VALUE;
    }
    if (_event != NULL) {
        CloseHandle(_event);
        _event = NULL;
    }
}

// An overlapped read or write with a timeout; false and a cancelled request on timeout.
static bool _io(bool write, uint8_t *buffer, unsigned timeout_ms) {
    OVERLAPPED overlapped = { .hEvent = _event };
    ResetEvent(_event);
    DWORD moved = 0;
    BOOL ok = write
        ? WriteFile(_handle, buffer, HID_REPORT_SIZE + 1, &moved, &overlapped)
        : ReadFile(_handle, buffer, HID_REPORT_SIZE + 1, &moved, &overlapped);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING) {
            _win_error(write ? "HID write" : "HID read");
            return false;
        }
        if (WaitForSingleObject(_event, timeout_ms) != WAIT_OBJECT_0) {
            CancelIo(_handle);
            GetOverlappedResult(_handle, &overlapped, &moved, TRUE);
            snprintf(_last_error, sizeof(_last_error), "%s: timed out", write ? "HID write" : "HID read");
            return false;
        }
        if (!GetOverlappedResult(_handle, &overlapped, &moved, FALSE)) {
            _win_error(write ? "HID write" : "HID read");
            return false;
        }
    }
    return true;
}

bool hid_host_send(const uint8_t cmd[HID_REPORT_SIZE]) {
    if (_handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    uint8_t out[HID_REPORT_SIZE + 1];
    out[0] = 0;     // the report id
    memcpy(out + 1, cmd, HID_REPORT_SIZE);
    return _io(true, out, 1000);
}

bool hid_host_exchange(const uint8_t cmd[HID_REPORT_SIZE], uint8_t reply[HID_REPORT_SIZE], unsigned timeout_ms) {
    if (!hid_host_send(cmd)) {
        return false;
    }
    uint8_t in[HID_REPORT_SIZE + 1];
    if (!_io(false, in, timeout_ms)) {
        return false;
    }
    memcpy(reply, in + 1, HID_REPORT_SIZE);
    return true;
}

