// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <string.h>

#include "common-hal/nvm/ByteArray.h"
#include "shared-bindings/nvm/ByteArray.h"

#include "desktop_port.h"

// The file's bytes, read at first use. A missing file, or the part past a short file's end,
// reads 0xFF, as erased flash does; a longer file's rest is left alone.
static uint8_t _nvm[CIRCUITPY_INTERNAL_NVM_SIZE];
static bool _loaded;

static void _load(void) {
    memset(_nvm, 0xff, sizeof(_nvm));
    FILE *f = fopen(desktop_options.nvm_path, "rb");
    if (f != NULL) {
        size_t n = fread(_nvm, 1, sizeof(_nvm), f);
        (void)n;
        fclose(f);
    }
    _loaded = true;
}

uint32_t common_hal_nvm_bytearray_get_length(const nvm_bytearray_obj_t *self) {
    return sizeof(_nvm);
}

void common_hal_nvm_bytearray_get_bytes(const nvm_bytearray_obj_t *self,
    uint32_t start_index, uint32_t len, uint8_t *values) {
    if (!_loaded) {
        _load();
    }
    memcpy(values, _nvm + start_index, len);
}

// Written through at once: the bytes are in the file when the call returns.
bool common_hal_nvm_bytearray_set_bytes(const nvm_bytearray_obj_t *self,
    uint32_t start_index, uint8_t *values, uint32_t len) {
    if (!_loaded) {
        _load();
    }
    memcpy(_nvm + start_index, values, len);
    bool ok;
    FILE *f = fopen(desktop_options.nvm_path, "r+b");
    if (f != NULL) {
        // Padded first when the file is shorter than the write's end.
        fseek(f, 0, SEEK_END);
        long end = ftell(f);
        ok = end >= 0;
        uint32_t size = ok ? (uint32_t)end : 0;
        if (ok && size < start_index) {
            ok = fwrite(_nvm + size, 1, start_index - size, f) == (size_t)(start_index - size);
        }
        ok = ok && fseek(f, start_index, SEEK_SET) == 0 && fwrite(values, 1, len, f) == len;
    } else {
        f = fopen(desktop_options.nvm_path, "wb");
        ok = f != NULL && fwrite(_nvm, 1, sizeof(_nvm), f) == sizeof(_nvm);
    }
    if (f != NULL && fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        // What the file holds after all.
        _load();
    }
    return ok;
}
