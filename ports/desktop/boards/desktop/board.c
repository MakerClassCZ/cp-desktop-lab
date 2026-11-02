// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>

#include "supervisor/board.h"
#include "supervisor/filesystem.h"
#include "supervisor/port.h"
#include "supervisor/shared/reload.h"
#include "common-hal/sdl/Framebuffer.h"
#include "shared-bindings/supervisor/__init__.h"

#include "ble_host.h"
#include "desktop_pins.h"
#include "desktop_port.h"

// --code FILE: the supervisor's next code file (as supervisor.set_next_code_file sets it), kept
// for every run, from the file's directory. A file that is not on the drive stops the start.
static void _code_file(void) {
    static bool done;
    if (desktop_options.code == NULL || done) {
        return;
    }
    done = true;
    const char *code = desktop_options.code;
    size_t len = strlen(code) + (code[0] != '/');
    bool is_dir = true;
    char path[256];
    if (len >= sizeof(path) || snprintf(path, sizeof(path), "%s%s", code[0] == '/' ? "" : "/", code) < 0
        || supervisor_vfs_stat(filesystem_circuitpy(), path, &is_dir, NULL, NULL) != SUPERVISOR_FS_OK || is_dir) {
        fprintf(stderr, "circuitpython: --code %s: no such file on the drive\n", code);
        desktop_power_off(2);
    }
    size_t dir = (size_t)(strrchr(path, '/') - path);
    supervisor_next_code_info_t *info = port_malloc(sizeof(*info) + 2 * (len + 1), false);
    if (info == NULL) {
        return;
    }
    char *filename = (char *)(info + 1);
    char *directory = filename + len + 1;
    memcpy(filename, path, len + 1);
    memcpy(directory, path, dir == 0 ? 1 : dir);
    directory[dir == 0 ? 1 : dir] = '\0';
    info->filename = filename;
    info->working_directory = directory;
    info->options = SUPERVISOR_NEXT_CODE_OPT_STICKY_ON_SUCCESS | SUPERVISOR_NEXT_CODE_OPT_STICKY_ON_ERROR
        | SUPERVISOR_NEXT_CODE_OPT_STICKY_ON_RELOAD | SUPERVISOR_NEXT_CODE_OPT_NEWLY_SET;
    next_code_configuration = info;
}

void board_init(void) {
    if (desktop_pin_backend->accepts_drive) {
        desktop_buttons_release_all();
    }
    // A 320x240 picture, or the board file's, in a window twice its size.
    sdl_framebuffer_autoconstruct(desktop_display_width, desktop_display_height, 2, false);
    ble_host_attach_adapter();
    _code_file();
}
