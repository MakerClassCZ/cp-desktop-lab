// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The drive image mapped with a file mapping, and statvfs() for a directory drive.

#include <errno.h>
#include <stdio.h>
#include <windows.h>

#include "sys/statvfs.h"

#include "desktop_port.h"
#include "platform.h"

uint8_t *platform_map_drive(const char *path, size_t new_size, size_t block, size_t *size) {
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER length = {0};
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &length)) {
        fprintf(stderr, "circuitpython: cannot open drive %s: error %lu\n", path, (unsigned long)GetLastError());
        desktop_power_off(1);
    }
    *size = length.QuadPart;
    if (*size == 0) {
        *size = new_size;
        length.QuadPart = *size;
        if (!SetFilePointerEx(file, length, NULL, FILE_BEGIN) || !SetEndOfFile(file)) {
            fprintf(stderr, "circuitpython: cannot size drive %s: error %lu\n", path, (unsigned long)GetLastError());
            desktop_power_off(1);
        }
    }
    *size -= *size % block;
    uint8_t *drive = NULL;
    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READWRITE, (DWORD)((uint64_t)*size >> 32), (DWORD)*size, NULL);
    if (mapping != NULL) {
        drive = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, *size);
        CloseHandle(mapping);
    }
    CloseHandle(file);
    if (drive == NULL) {
        fprintf(stderr, "circuitpython: cannot map drive %s: error %lu\n", path, (unsigned long)GetLastError());
        desktop_power_off(1);
    }
    return drive;
}

void platform_flush_drive(uint8_t *drive, size_t size) {
    FlushViewOfFile(drive, size);
}

int statvfs(const char *path, struct statvfs *sb) {
    char root[MAX_PATH];
    DWORD sectors, sector_size, free_clusters, clusters;
    ULARGE_INTEGER avail, total, unused;
    if (!GetVolumePathNameA(path, root, sizeof(root))
        || !GetDiskFreeSpaceA(root, &sectors, &sector_size, &free_clusters, &clusters)
        || !GetDiskFreeSpaceExA(path, &avail, &total, &unused)) {
        DWORD error = GetLastError();
        errno = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ENOENT : EIO;
        return -1;
    }
    // GetDiskFreeSpaceA caps its counts at 2 GB on big volumes; the Ex byte counts are exact.
    unsigned long block = sectors * sector_size;
    *sb = (struct statvfs) {
        .f_bsize = block,
        .f_frsize = block,
        .f_blocks = total.QuadPart / block,
        .f_bfree = unused.QuadPart / block,
        .f_bavail = avail.QuadPart / block,
        .f_namemax = MAX_PATH,
    };
    return 0;
}
