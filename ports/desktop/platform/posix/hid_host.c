// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "hid_host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static char _last_error[128];

const char *hid_host_last_error(void) {
    return _last_error;
}

static int _fd = -1;

bool hid_host_open(uint16_t vid, uint16_t pid, const char *what, char *err, size_t err_len) {
    if (_fd >= 0) {
        return true;
    }
    char wanted[64];
    snprintf(wanted, sizeof(wanted), "HID_ID=0003:%08X:%08X", vid, pid);
    DIR *dir = opendir("/sys/class/hidraw");
    if (dir == NULL) {
        snprintf(err, err_len, "no %s found (VID %04x PID %04x): no hidraw support", what, vid, pid);
        return false;
    }
    bool found = false;
    int open_errno = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "hidraw", 6) != 0) {
            continue;
        }
        char path[512];
        snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", entry->d_name);
        FILE *uevent = fopen(path, "r");
        if (uevent == NULL) {
            continue;
        }
        bool match = false;
        char line[128];
        while (fgets(line, sizeof(line), uevent) != NULL) {
            if (strncmp(line, wanted, strlen(wanted)) == 0) {
                match = true;
            }
        }
        fclose(uevent);
        if (!match) {
            continue;
        }
        found = true;
        snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
        _fd = open(path, O_RDWR | O_CLOEXEC);
        // hidraw lets many open a device; a lock keeps a second CircuitPython off this one.
        if (_fd >= 0 && flock(_fd, LOCK_EX | LOCK_NB) == 0) {
            break;
        }
        open_errno = _fd >= 0 ? EBUSY : errno;
        if (_fd >= 0) {
            close(_fd);
            _fd = -1;
        }
    }
    closedir(dir);
    if (!found) {
        snprintf(err, err_len, "no %s found (VID %04x PID %04x)", what, vid, pid);
        return false;
    }
    if (_fd < 0) {
        snprintf(err, err_len, "cannot open %s: %s%s", what, strerror(open_errno),
            open_errno == EBUSY ? " (another program has it)" : "");
        return false;
    }
    return true;
}

bool hid_host_present(uint16_t vid, uint16_t pid) {
    char wanted[64];
    snprintf(wanted, sizeof(wanted), "HID_ID=0003:%08X:%08X", vid, pid);
    DIR *dir = opendir("/sys/class/hidraw");
    if (dir == NULL) {
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        char path[512];
        snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", entry->d_name);
        FILE *uevent = fopen(path, "r");
        if (uevent == NULL) {
            continue;
        }
        char line[128];
        while (fgets(line, sizeof(line), uevent) != NULL) {
            if (strncmp(line, wanted, strlen(wanted)) == 0) {
                found = true;
            }
        }
        fclose(uevent);
    }
    closedir(dir);
    return found;
}

void hid_host_close(void) {
    if (_fd >= 0) {
        close(_fd);
        _fd = -1;
    }
}

bool hid_host_send(const uint8_t cmd[HID_REPORT_SIZE]) {
    if (_fd < 0) {
        return false;
    }
    uint8_t out[HID_REPORT_SIZE + 1];
    out[0] = 0;     // the report number; hidraw drops it for a device without numbered reports
    memcpy(out + 1, cmd, HID_REPORT_SIZE);
    if (write(_fd, out, sizeof(out)) != (ssize_t)sizeof(out)) {
        snprintf(_last_error, sizeof(_last_error), "HID write: %s", strerror(errno));
        return false;
    }
    return true;
}

bool hid_host_exchange(const uint8_t cmd[HID_REPORT_SIZE], uint8_t reply[HID_REPORT_SIZE], unsigned timeout_ms) {
    if (!hid_host_send(cmd)) {
        return false;
    }
    struct pollfd pfd = { .fd = _fd, .events = POLLIN };
    int ready = poll(&pfd, 1, (int)timeout_ms);
    if (ready <= 0) {
        snprintf(_last_error, sizeof(_last_error), "HID read: %s", ready == 0 ? "timed out" : strerror(errno));
        return false;
    }
    ssize_t got = read(_fd, reply, HID_REPORT_SIZE);
    if (got != HID_REPORT_SIZE) {
        snprintf(_last_error, sizeof(_last_error), "HID read: %s", got < 0 ? strerror(errno) : "short report");
        return false;
    }
    return true;
}

