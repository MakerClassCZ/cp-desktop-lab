// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The drive image mapped with mmap.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "desktop_port.h"
#include "platform.h"

uint8_t *platform_map_drive(const char *path, size_t new_size, size_t block, size_t *size) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "circuitpython: cannot open drive %s: %s\n", path, strerror(errno));
        desktop_power_off(1);
    }
    *size = st.st_size;
    if (*size == 0) {
        *size = new_size;
        if (ftruncate(fd, *size) != 0) {
            fprintf(stderr, "circuitpython: cannot size drive %s: %s\n", path, strerror(errno));
            desktop_power_off(1);
        }
    }
    *size -= *size % block;
    uint8_t *drive = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (drive == MAP_FAILED) {
        fprintf(stderr, "circuitpython: cannot map drive %s: %s\n", path, strerror(errno));
        desktop_power_off(1);
    }
    return drive;
}

void platform_flush_drive(uint8_t *drive, size_t size) {
    msync(drive, size, MS_ASYNC);
}
