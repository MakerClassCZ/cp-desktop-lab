// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "serial_host.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>

#define H(port) ((HANDLE)(port)->handle)

bool serial_host_open(serial_host_t *port, const char *path, char *err, size_t err_len) {
    char name[64];
    // COM10 and above open only by their device name.
    if (_strnicmp(path, "COM", 3) == 0) {
        snprintf(name, sizeof(name), "\\\\.\\%s", path);
    } else {
        snprintf(name, sizeof(name), "%s", path);
    }
    HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        snprintf(err, err_len, "cannot open %s: %s", path,
            e == ERROR_ACCESS_DENIED ? "another program has it" : e == ERROR_FILE_NOT_FOUND ? "no such port" : "error");
        port->handle = -1;
        return false;
    }
    port->handle = (intptr_t)h;
    return true;
}

void serial_host_close(serial_host_t *port) {
    if (port->handle != -1) {
        CloseHandle(H(port));
        port->handle = -1;
    }
}

bool serial_host_configure(serial_host_t *port, uint32_t baud, uint8_t bits, char parity, uint8_t stop) {
    DCB dcb = { .DCBlength = sizeof(DCB) };
    if (!GetCommState(H(port), &dcb)) {
        return false;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = bits;
    dcb.Parity = parity == 'E' ? EVENPARITY : parity == 'O' ? ODDPARITY : NOPARITY;
    dcb.fParity = parity != 'N';
    // Two stop bits with five data bits are one and a half to Windows.
    dcb.StopBits = stop == 2 ? (bits == 5 ? ONE5STOPBITS : TWOSTOPBITS) : ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    return SetCommState(H(port), &dcb);
}

int serial_host_read(serial_host_t *port, uint8_t *data, size_t len, int timeout_ms) {
    // Back as soon as a byte is there, or after timeout_ms without one.
    COMMTIMEOUTS t = { 0 };
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = timeout_ms > 0 ? MAXDWORD : 0;
    t.ReadTotalTimeoutConstant = timeout_ms > 0 ? (DWORD)timeout_ms : 0;
    t.WriteTotalTimeoutConstant = 2000;
    if (!SetCommTimeouts(H(port), &t)) {
        return -1;
    }
    DWORD n = 0;
    if (!ReadFile(H(port), data, (DWORD)len, &n, NULL)) {
        return -1;
    }
    return (int)n;
}

int serial_host_write(serial_host_t *port, const uint8_t *data, size_t len, int timeout_ms) {
    COMMTIMEOUTS t = { 0 };
    t.ReadIntervalTimeout = MAXDWORD;
    t.WriteTotalTimeoutConstant = (DWORD)timeout_ms;
    if (!SetCommTimeouts(H(port), &t)) {
        return -1;
    }
    DWORD n = 0;
    if (!WriteFile(H(port), data, (DWORD)len, &n, NULL)) {
        return -1;
    }
    return (int)n;
}

int serial_host_available(serial_host_t *port) {
    COMSTAT stat;
    DWORD errors;
    if (!ClearCommError(H(port), &errors, &stat)) {
        return -1;
    }
    return (int)stat.cbInQue;
}

void serial_host_flush_input(serial_host_t *port) {
    PurgeComm(H(port), PURGE_RXCLEAR);
}

// The ports class: a device whose hardware ID names the VID and PID, its PortName from the registry.
bool serial_host_find_usb(uint16_t vid, uint16_t pid, char *path, size_t len) {
    char want[32];
    snprintf(want, sizeof(want), "VID_%04X&PID_%04X", vid, pid);
    HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    SP_DEVINFO_DATA dev = { .cbSize = sizeof(SP_DEVINFO_DATA) };
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        char ids[1024] = { 0 };
        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_HARDWAREID, NULL, (BYTE *)ids, sizeof(ids) - 2, NULL)) {
            continue;
        }
        bool match = false;
        for (char *id = ids; *id != '\0'; id += strlen(id) + 1) {
            if (strstr(CharUpperA(id), want) != NULL) {
                match = true;
            }
        }
        if (!match) {
            continue;
        }
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) {
            continue;
        }
        DWORD size = (DWORD)len;
        DWORD type = 0;
        if (RegQueryValueExA(key, "PortName", NULL, &type, (BYTE *)path, &size) == ERROR_SUCCESS && type == REG_SZ) {
            path[len - 1] = '\0';
            found = true;
        }
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}
