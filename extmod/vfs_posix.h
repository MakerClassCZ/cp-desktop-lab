/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef MICROPY_INCLUDED_EXTMOD_VFS_POSIX_H
#define MICROPY_INCLUDED_EXTMOD_VFS_POSIX_H

#include "py/lexer.h"
#include "py/obj.h"
// CIRCUITPY-CHANGE
#include "extmod/vfs.h"

// CIRCUITPY-CHANGE: public, so the supervisor can serve CIRCUITPY from a host directory. It then
// starts like the other supervisor filesystems: the base, then a blockdev that only holds the
// supervisor's flags (there is no block device behind a directory).
typedef struct _mp_obj_vfs_posix_t {
    mp_obj_base_t base;
    #if defined(CIRCUITPY_FILESYSTEM_POSIX) && CIRCUITPY_FILESYSTEM_POSIX
    mp_vfs_blockdev_t blockdev;
    #endif
    vstr_t root;
    size_t root_len;
    bool readonly;
} mp_obj_vfs_posix_t;

extern const mp_obj_type_t mp_type_vfs_posix;
extern const mp_obj_type_t mp_type_vfs_posix_fileio;
extern const mp_obj_type_t mp_type_vfs_posix_textio;

mp_obj_t mp_vfs_posix_file_open(const mp_obj_type_t *type, mp_obj_t file_in, mp_obj_t mode_in);

// CIRCUITPY-CHANGE
#if defined(CIRCUITPY_FILESYSTEM_POSIX) && CIRCUITPY_FILESYSTEM_POSIX
// Set up vfs for the absolute directory root without the GC heap; buf holds the root and the
// paths joined to it.
void mp_vfs_posix_init_static(mp_obj_vfs_posix_t *vfs, const char *root, char *buf, size_t buf_len);
// The descriptor of an open VfsPosix file, -1 once it is closed.
int mp_vfs_posix_file_fd(mp_obj_t file_in);
#endif

#endif // MICROPY_INCLUDED_EXTMOD_VFS_POSIX_H
