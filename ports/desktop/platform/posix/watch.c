// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// A directory CIRCUITPY reloads code.py when the host changes a file in it, as a board does when
// a computer writes to its drive. Hidden files and directories (editor swap files, .git) do not
// count, nor does boot_out.txt, nor anything while Python itself may write the drive.
// This system's watcher: Linux's inotify, a watch on each directory.

#define _XOPEN_SOURCE_EXTENDED 1

#include "py/mpconfig.h"

#if CIRCUITPY_FILESYSTEM_POSIX

#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "desktop_port.h"

#define WATCH_MASK (IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF)
#define WATCHES_MAX 1024

static int _fd = -1;
static int _root_wd = -1;
static char *_paths[WATCHES_MAX];       // watch descriptor -> directory path

static void _watch_directory(const char *path) {
    int wd = inotify_add_watch(_fd, path, WATCH_MASK | IN_ONLYDIR);
    if (wd >= 0 && wd < WATCHES_MAX && _paths[wd] == NULL) {
        _paths[wd] = strdup(path);
    }
    if (_root_wd < 0) {
        _root_wd = wd;
    }
}

static int _visit(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)st;
    if (type != FTW_D) {
        return FTW_CONTINUE;
    }
    if (ftw->level > 0 && path[ftw->base] == '.') {
        return FTW_SKIP_SUBTREE;
    }
    _watch_directory(path);
    return FTW_CONTINUE;
}

static void *_watch(void *arg) {
    nftw((const char *)arg, _visit, 16, FTW_ACTIONRETVAL | FTW_PHYS);
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(_fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return NULL;
        }
        bool changed = false;
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *event = (struct inotify_event *)p;
            p += sizeof(*event) + event->len;
            if (event->len == 0 || event->name[0] == '.') {
                continue;
            }
            if (event->wd == _root_wd && strcmp(event->name, "boot_out.txt") == 0) {
                continue;
            }
            if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO)) &&
                event->wd >= 0 && event->wd < WATCHES_MAX && _paths[event->wd] != NULL) {
                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%s/%s", _paths[event->wd], event->name);
                nftw(path, _visit, 16, FTW_ACTIONRETVAL | FTW_PHYS);
            }
            changed = true;
        }
        if (changed) {
            desktop_watch_changed();
        }
    }
}

void desktop_watch_start(const char *root) {
    if (_fd >= 0) {
        return;
    }
    _fd = inotify_init1(IN_CLOEXEC);
    if (_fd < 0) {
        perror("circuitpython: inotify");
        return;
    }
    desktop_watch_thread(_watch, (void *)root);
}

#endif
