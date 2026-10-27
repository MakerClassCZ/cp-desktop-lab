// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// A directory CIRCUITPY reloads code.py when the host changes a file in it, as a board does when
// a computer writes to its drive. Hidden files and directories (editor swap files, .git) do not
// count, nor does boot_out.txt, nor anything while Python itself may write the drive.
// This system's watcher: macOS's FSEvents, one stream over the whole tree with an event per file.

#include "py/mpconfig.h"

#if CIRCUITPY_FILESYSTEM_POSIX

#include <CoreServices/CoreServices.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "desktop_port.h"

static char _root[PATH_MAX];
static size_t _root_len;

// A path below the root that names a hidden file or directory, or the root's boot_out.txt.
static bool _ignored(const char *path) {
    if (strncmp(path, _root, _root_len) != 0 || path[_root_len] != '/') {
        return false;
    }
    const char *rel = path + _root_len + 1;
    if (strcmp(rel, "boot_out.txt") == 0) {
        return true;
    }
    for (const char *p = rel; *p != '\0'; p++) {
        if ((p == rel || p[-1] == '/') && *p == '.') {
            return true;
        }
    }
    return false;
}

static void _events(ConstFSEventStreamRef stream, void *info, size_t count, void *paths,
    const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]) {
    (void)stream;
    (void)info;
    (void)ids;
    bool changed = false;
    for (size_t i = 0; i < count; i++) {
        const char *path = ((const char **)paths)[i];
        // The root's own events (it was created or its parent watched) name no file in it.
        if ((flags[i] & kFSEventStreamEventFlagItemIsFile) || (flags[i] & kFSEventStreamEventFlagItemIsDir)) {
            if (strcmp(path, _root) != 0 && !_ignored(path)) {
                changed = true;
            }
        } else if (!_ignored(path)) {
            changed = true;     // events were merged into one for the directory
        }
    }
    if (changed) {
        desktop_watch_changed();
    }
}

void desktop_watch_start(const char *root) {
    if (_root_len > 0) {
        return;
    }
    // FSEvents reports paths with symbolic links resolved (/tmp is /private/tmp).
    if (realpath(root, _root) == NULL) {
        return;
    }
    _root_len = strlen(_root);
    CFStringRef path = CFStringCreateWithCString(NULL, _root, kCFStringEncodingUTF8);
    CFArrayRef paths = CFArrayCreate(NULL, (const void **)&path, 1, &kCFTypeArrayCallBacks);
    FSEventStreamRef stream = FSEventStreamCreate(NULL, _events, NULL, paths,
        kFSEventStreamEventIdSinceNow, 0.05,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths);
    CFRelease(path);
    if (stream == NULL) {
        return;
    }
    // The events come on a queue of their own, as a watcher's thread on the other systems.
    FSEventStreamSetDispatchQueue(stream, dispatch_queue_create("circuitpython.watch", NULL));
    FSEventStreamStart(stream);
}

#endif
