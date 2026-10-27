// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "serial_host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define FD(port) ((int)(port)->handle)

bool serial_host_open(serial_host_t *port, const char *path, char *err, size_t err_len) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, err_len, "cannot open %s: %s", path, strerror(errno));
        port->handle = -1;
        return false;
    }
    port->handle = fd;
    return true;
}

void serial_host_close(serial_host_t *port) {
    if (port->handle != -1) {
        close(FD(port));
        port->handle = -1;
    }
}

static speed_t _speed(uint32_t baud) {
    static const struct { uint32_t baud;
                          speed_t speed;
    } speeds[] = {
        { 300, B300 }, { 600, B600 }, { 1200, B1200 }, { 2400, B2400 }, { 4800, B4800 },
        { 9600, B9600 }, { 19200, B19200 }, { 38400, B38400 }, { 57600, B57600 },
        { 115200, B115200 }, { 230400, B230400 },
        #ifdef B460800
        { 460800, B460800 }, { 500000, B500000 }, { 576000, B576000 }, { 921600, B921600 },
        { 1000000, B1000000 }, { 1500000, B1500000 }, { 2000000, B2000000 },
        #endif
    };
    for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        if (speeds[i].baud == baud) {
            return speeds[i].speed;
        }
    }
    return 0;
}

bool serial_host_configure(serial_host_t *port, uint32_t baud, uint8_t bits, char parity, uint8_t stop) {
    speed_t speed = _speed(baud);
    struct termios t;
    if (speed == 0 || tcgetattr(FD(port), &t) != 0) {
        return false;
    }
    cfmakeraw(&t);
    cfsetispeed(&t, speed);
    cfsetospeed(&t, speed);
    t.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
    #ifdef CRTSCTS
    t.c_cflag &= ~CRTSCTS;
    #endif
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag |= bits == 5 ? CS5 : bits == 6 ? CS6 : bits == 7 ? CS7 : CS8;
    if (parity != 'N') {
        t.c_cflag |= PARENB | (parity == 'O' ? PARODD : 0);
    }
    if (stop == 2) {
        t.c_cflag |= CSTOPB;
    }
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    return tcsetattr(FD(port), TCSANOW, &t) == 0;
}

int serial_host_read(serial_host_t *port, uint8_t *data, size_t len, int timeout_ms) {
    struct pollfd p = { .fd = FD(port), .events = POLLIN };
    int r = poll(&p, 1, timeout_ms);
    if (r < 0) {
        return errno == EINTR ? 0 : -1;
    }
    if (r == 0) {
        return 0;
    }
    ssize_t n = read(FD(port), data, len);
    if (n < 0) {
        return errno == EAGAIN || errno == EINTR ? 0 : -1;
    }
    // Ready but nothing to read: the other end hung up (a pty closed, a USB adapter pulled).
    if (n == 0 || (p.revents & (POLLHUP | POLLERR))) {
        return n > 0 ? (int)n : -1;
    }
    return (int)n;
}

int serial_host_write(serial_host_t *port, const uint8_t *data, size_t len, int timeout_ms) {
    size_t done = 0;
    while (done < len) {
        struct pollfd p = { .fd = FD(port), .events = POLLOUT };
        int r = poll(&p, 1, timeout_ms);
        if (r <= 0) {
            break;
        }
        ssize_t n = write(FD(port), data + done, len - done);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            return done > 0 ? (int)done : -1;
        }
        done += n;
    }
    return (int)done;
}

int serial_host_available(serial_host_t *port) {
    int n = 0;
    return ioctl(FD(port), FIONREAD, &n) == 0 ? n : -1;
}

void serial_host_flush_input(serial_host_t *port) {
    tcflush(FD(port), TCIFLUSH);
}

#ifndef __APPLE__     // macOS finds its ports through IOKit (platform/macos/serial_find.c)
static unsigned _read_hex(const char *path) {
    FILE *f = fopen(path, "r");
    unsigned v = 0;
    if (f != NULL) {
        if (fscanf(f, "%x", &v) != 1) {
            v = 0;
        }
        fclose(f);
    }
    return v;
}

// A CDC device is a ttyACM whose USB device (one above its interface in sysfs) has the IDs.
bool serial_host_find_usb(uint16_t vid, uint16_t pid, char *path, size_t len) {
    DIR *dir = opendir("/sys/class/tty");
    if (dir == NULL) {
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "ttyACM", 6) != 0) {
            continue;
        }
        char file[512];
        snprintf(file, sizeof(file), "/sys/class/tty/%s/device/../idVendor", entry->d_name);
        unsigned v = _read_hex(file);
        snprintf(file, sizeof(file), "/sys/class/tty/%s/device/../idProduct", entry->d_name);
        unsigned p = _read_hex(file);
        if (v == vid && p == pid) {
            snprintf(path, len, "/dev/%s", entry->d_name);
            found = true;
        }
    }
    closedir(dir);
    return found;
}
#endif
