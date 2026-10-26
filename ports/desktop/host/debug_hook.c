// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// With --debug, the debug adapter (python/debugadapter.py, kept beside the program) is loaded
// into the fresh VM as a module and waits for the editor before the user's code runs, so a
// breakpoint on the first line of code.py is hit too. The adapter installs sys.settrace and
// returns.

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/compile.h"
#include "py/lexer.h"
#include "py/nlr.h"
#include "py/objmodule.h"
#include "py/runtime.h"
#include "supervisor/port.h"

#include "desktop_port.h"

// The adapter's source, read from beside the program: the VM's own filesystem is the drive.
static char *read_adapter(size_t *len) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/debugadapter.py", desktop_options.exe_dir);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "circuitpython: %s is missing\r\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = size > 0 ? malloc(size) : NULL;
    if (source == NULL || fread(source, 1, size, f) != (size_t)size) {
        fprintf(stderr, "circuitpython: cannot read %s\r\n", path);
        free(source);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = size;
    return source;
}

void port_before_user_code(void) {
    if (desktop_options.debug_port <= 0) {
        return;
    }
    size_t len;
    char *source = read_adapter(&len);
    if (source == NULL) {
        return;
    }
    // The source is freed once, on the way that compiled it: a local changed after nlr_push()
    // is stale after an exception, so the failure path must not touch it.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        // The source becomes the module debugadapter, as an import would make it.
        mp_obj_t module = mp_obj_new_module(MP_QSTR_debugadapter);
        mp_obj_dict_t *globals = mp_obj_module_get_globals(module);
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR_debugadapter_dot_py, source, len, 0);
        mp_parse_compile_execute(lex, MP_PARSE_FILE_INPUT, globals, globals);
        free(source);
        mp_obj_t start = mp_load_attr(module, MP_QSTR_start);
        const char *drive = desktop_options.drive_path;
        mp_call_function_2(start, MP_OBJ_NEW_SMALL_INT(desktop_options.debug_port),
            mp_obj_new_str(drive, strlen(drive)));
        nlr_pop();
    } else {
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
        fprintf(stderr, "circuitpython: the debug adapter did not start; running without it\r\n");
    }
}
