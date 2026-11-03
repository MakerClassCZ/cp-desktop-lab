// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// An SD card in SPI mode, byte by byte as the host clocks: the commands the drivers send (CMD0,
// CMD8, ACMD41, CMD58, CMD9, CMD16, CMD59, CMD17/18 reads, CMD24/25 writes, CMD12, CMD13), an SDHC
// card's answers, its blocks in an image file.

#include "sdcard_virtual.h"

#include <stdio.h>
#include <string.h>

#include "py/mpconfig.h"
#include "extmod/vfs.h"
#include "extmod/vfs_fat.h"
#include "lib/oofatfs/ff.h"

#include "bus_trace.h"
#include "common-hal/microcontroller/Pin.h"
#include "desktop_pins.h"
#include "desktop_port.h"
#include "panel.h"
#include "parts.h"
#include "spi_virtual.h"

#define CARDS_MAX 2
#define BLOCK 512
#define CARD_MB 16
#define QUEUE 600

typedef enum {
    COMMAND,        // waiting for a command
    WRITE_TOKEN,    // after CMD24 or CMD25: waiting for a data token
    WRITE_DATA,     // a block coming in
    READING,        // CMD18: blocks going out until CMD12
} card_mode_t;

typedef struct {
    int cs;
    FILE *image;
    uint32_t blocks;
    card_mode_t mode;
    bool multi;             // CMD25: tokens until the stop token
    bool app;               // CMD55 came: the next command is an ACMD
    bool idle;
    uint8_t cmd[6];
    int cmd_len;
    uint32_t block;         // the next block to read or write
    uint8_t data[BLOCK + 2];
    int data_len;
    uint8_t queue[QUEUE];   // what the card sends next; 0xff once it is empty
    int q_head, q_len;
} card_t;

static card_t _cards[CARDS_MAX];
static int _count;

static void _send(card_t *c, const uint8_t *bytes, int n) {
    for (int i = 0; i < n && c->q_len < QUEUE; i++) {
        c->queue[(c->q_head + c->q_len++) % QUEUE] = bytes[i];
    }
}

static void _send1(card_t *c, uint8_t b) {
    _send(c, &b, 1);
}

static uint8_t _next(card_t *c) {
    if (c->q_len == 0) {
        return 0xff;
    }
    uint8_t b = c->queue[c->q_head];
    c->q_head = (c->q_head + 1) % QUEUE;
    c->q_len--;
    return b;
}

// ---- the image ----

static FILE *_image_file;   // the image being formatted

static mp_uint_t _read_blocks(mp_obj_t self, uint8_t *dest, uint32_t block, uint32_t n) {
    return fseek(_image_file, (long)block * BLOCK, SEEK_SET) == 0 && fread(dest, BLOCK, n, _image_file) == n ? 0 : 1;
}

static mp_uint_t _write_blocks(mp_obj_t self, const uint8_t *src, uint32_t block, uint32_t n) {
    return fseek(_image_file, (long)block * BLOCK, SEEK_SET) == 0 && fwrite(src, BLOCK, n, _image_file) == n ? 0 : 1;
}

static bool _ioctl(mp_obj_t self, size_t cmd, size_t arg, mp_int_t *out) {
    if (out != NULL) {
        *out = 0;
    }
    if (cmd == MP_BLOCKDEV_IOCTL_BLOCK_COUNT) {
        *out = CARD_MB * 1024 * 1024 / BLOCK;
    } else if (cmd == MP_BLOCKDEV_IOCTL_BLOCK_SIZE) {
        *out = BLOCK;
    }
    return true;
}

// The card's image: opened, or made and formatted (FAT, no partition table) as a new card is.
static FILE *_open_image(uint32_t *blocks) {
    char path[1100];
    snprintf(path, sizeof(path), "%s-SD.img", desktop_options.drive_path);
    FILE *f = fopen(path, "r+b");
    if (f == NULL) {
        f = fopen(path, "w+b");
        if (f == NULL) {
            return NULL;
        }
        // Its full size, the blocks never written reading as zeros.
        static const uint8_t zero[BLOCK];
        fseek(f, (long)CARD_MB * 1024 * 1024 - BLOCK, SEEK_SET);
        fwrite(zero, 1, BLOCK, f);
        static fs_user_mount_t vfs;
        memset(&vfs, 0, sizeof(vfs));
        vfs.blockdev.flags = MP_BLOCKDEV_FLAG_NATIVE | MP_BLOCKDEV_FLAG_HAVE_IOCTL;
        vfs.blockdev.block_size = BLOCK;
        // [0] is none, not NULL: a NULL writeblocks[0] is a read-only device.
        vfs.blockdev.readblocks[0] = mp_const_none;
        vfs.blockdev.writeblocks[0] = mp_const_none;
        vfs.blockdev.u.ioctl[0] = mp_const_none;
        vfs.blockdev.readblocks[1] = (mp_obj_t)&vfs.blockdev;
        vfs.blockdev.readblocks[2] = (mp_obj_t)_read_blocks;
        vfs.blockdev.writeblocks[1] = (mp_obj_t)&vfs.blockdev;
        vfs.blockdev.writeblocks[2] = (mp_obj_t)_write_blocks;
        vfs.blockdev.u.ioctl[1] = (mp_obj_t)&vfs.blockdev;
        vfs.blockdev.u.ioctl[2] = (mp_obj_t)_ioctl;
        vfs.fatfs.drv = &vfs;
        static uint8_t work[FF_MAX_SS];
        _image_file = f;
        FRESULT res = f_mkfs(&vfs.fatfs, FM_FAT | FM_SFD, 0, work, sizeof(work));
        _image_file = NULL;
        fflush(f);
        if (res != FR_OK) {
            fprintf(stderr, "virtual: the SD card's image %s: no file system made (FatFs %d)\r\n", path, (int)res);
            fclose(f);
            remove(path);
            return NULL;
        }
        fprintf(stderr, "virtual: a new SD card, %d MB, at %s\r\n", CARD_MB, path);
    }
    fseek(f, 0, SEEK_END);
    *blocks = (uint32_t)(ftell(f) / BLOCK);
    return f;
}

bool sdcard_virtual_add(int cs, const char **why) {
    for (int i = 0; i < _count; i++) {
        if (_cards[i].cs == cs) {
            return true;
        }
    }
    if (_count == CARDS_MAX) {
        *why = "more than 2 SD cards";
        return false;
    }
    card_t *c = &_cards[_count];
    memset(c, 0, sizeof(*c));
    c->cs = cs;
    c->idle = true;
    _count++;
    return true;
}

static bool _low(int pin) {
    bool output, level;
    return desktop_virtual_pin((uint8_t)pin, &output, &level) && output && !level;
}

int sdcard_virtual_selected(void) {
    for (int i = 0; i < _count; i++) {
        if (_low(_cards[i].cs)) {
            return i;
        }
    }
    return -1;
}

// ---- the protocol ----

static void _r1(card_t *c, uint8_t r1) {
    uint8_t answer[2] = { 0xff, r1 };     // one byte's wait (NCR), then the answer
    _send(c, answer, 2);
}

static void _block_out(card_t *c, uint32_t block) {
    uint8_t data[BLOCK];
    memset(data, 0, sizeof(data));
    if (block < c->blocks && fseek(c->image, (long)block * BLOCK, SEEK_SET) == 0) {
        size_t got = fread(data, 1, BLOCK, c->image);
        (void)got;
    }
    _send1(c, 0xff);
    _send1(c, 0xfe);                        // the data token
    _send(c, data, BLOCK);
    uint8_t crc[2] = { 0, 0 };              // not checked by the drivers
    _send(c, crc, 2);
}

// CSD version 2: an SDHC card of c->blocks blocks (C_SIZE counts 512 KB).
static void _csd(card_t *c) {
    uint32_t c_size = c->blocks / 1024 - 1;
    uint8_t csd[16] = { 0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00, (uint8_t)(c_size >> 16 & 0x3f),
                        (uint8_t)(c_size >> 8), (uint8_t)c_size, 0x7f, 0x80, 0x0a, 0x40, 0x00, 0x01 };
    _r1(c, 0x00);
    _send1(c, 0xff);
    _send1(c, 0xfe);
    _send(c, csd, sizeof(csd));
    uint8_t crc[2] = { 0, 0 };
    _send(c, crc, 2);
}

static void _command(card_t *c) {
    uint8_t cmd = c->cmd[0] & 0x3f;
    uint32_t arg = (uint32_t)c->cmd[1] << 24 | c->cmd[2] << 16 | c->cmd[3] << 8 | c->cmd[4];
    bool app = c->app;
    c->app = false;
    if (c->mode == READING && cmd != 12) {
        c->mode = COMMAND;
    }
    c->q_len = 0;   // a command interrupts what the card was sending
    if (app && cmd == 41) {
        c->idle = false;
        _r1(c, 0x00);
        return;
    }
    switch (cmd) {
        case 0:
            c->idle = true;
            c->mode = COMMAND;
            _r1(c, 0x01);
            break;
        case 8: {
            uint8_t r7[4] = { 0x00, 0x00, 0x01, (uint8_t)arg };
            _r1(c, c->idle ? 0x01 : 0x00);
            _send(c, r7, 4);
            break;
        }
        case 55:
            c->app = true;
            _r1(c, c->idle ? 0x01 : 0x00);
            break;
        case 58: {
            uint8_t ocr[4] = { 0xc0, 0xff, 0x80, 0x00 };   // powered up, SDHC (block addresses)
            _r1(c, c->idle ? 0x01 : 0x00);
            _send(c, ocr, 4);
            break;
        }
        case 9:
            _csd(c);
            break;
        case 12:
            c->mode = COMMAND;
            _r1(c, 0x00);
            break;
        case 13: {
            _r1(c, 0x00);
            _send1(c, 0x00);
            break;
        }
        case 16:
        case 59:
            _r1(c, 0x00);
            break;
        case 17:
            _r1(c, 0x00);
            _block_out(c, arg);
            bus_trace_text("SD read %lu", (unsigned long)arg);
            break;
        case 18:
            _r1(c, 0x00);
            c->mode = READING;
            c->block = arg;
            _block_out(c, c->block++);
            bus_trace_text("SD read %lu...", (unsigned long)arg);
            break;
        case 24:
        case 25:
            _r1(c, 0x00);
            c->mode = WRITE_TOKEN;
            c->multi = cmd == 25;
            c->block = arg;
            break;
        default:
            _r1(c, 0x04);   // illegal command
            break;
    }
}

static void _byte_in(card_t *c, uint8_t b) {
    switch (c->mode) {
        case WRITE_TOKEN:
            if (b == 0xfe || (c->multi && b == 0xfc)) {
                c->mode = WRITE_DATA;
                c->data_len = 0;
            } else if (c->multi && b == 0xfd) {
                // The stop token: busy a moment, then ready.
                uint8_t busy[2] = { 0xff, 0x00 };
                _send(c, busy, 2);
                c->mode = COMMAND;
            } else if ((b & 0xc0) == 0x40) {
                c->mode = COMMAND;
                c->cmd[0] = b;
                c->cmd_len = 1;
            }
            return;
        case WRITE_DATA:
            c->data[c->data_len++] = b;
            if (c->data_len == BLOCK + 2) {
                bool ok = c->block < c->blocks && fseek(c->image, (long)c->block * BLOCK, SEEK_SET) == 0
                    && fwrite(c->data, 1, BLOCK, c->image) == BLOCK;
                fflush(c->image);
                bus_trace_text("SD write %lu", (unsigned long)c->block);
                c->block++;
                uint8_t answer[3] = { ok ? 0xe5 : 0xed, 0x00, 0x00 };   // accepted (or a write error), busy
                _send(c, answer, 3);
                c->mode = c->multi ? WRITE_TOKEN : COMMAND;
            }
            return;
        default:
            break;
    }
    if (c->cmd_len == 0 && (b & 0xc0) != 0x40) {
        return;     // 0xff while the host waits, or reads what is queued
    }
    c->cmd[c->cmd_len++] = b;
    if (c->cmd_len == 6) {
        c->cmd_len = 0;
        _command(c);
    }
}

void sdcard_virtual_transfer(int card, const uint8_t *out, uint8_t fill, uint8_t *in, size_t len) {
    card_t *c = &_cards[card];
    // The image at the card's first use, not when a board file names it.
    if (c->image == NULL) {
        c->image = _open_image(&c->blocks);
        if (c->image == NULL) {
            fprintf(stderr, "virtual: cannot make the SD card's image beside the drive\r\n");
            if (in != NULL) {
                memset(in, 0xff, len);  // no card answers
            }
            return;
        }
    }
    for (size_t i = 0; i < len; i++) {
        if (c->mode == READING && c->q_len == 0) {
            _block_out(c, c->block++);
        }
        uint8_t answer = _next(c);
        if (in != NULL) {
            in[i] = answer;
        }
        _byte_in(c, out != NULL ? out[i] : fill);
    }
}

bool sdcard_virtual_attach_default(const uint8_t *out, size_t len) {
    static const uint8_t cmd0[6] = { 0x40, 0, 0, 0, 0, 0x95 };
    bool reset = false;
    for (size_t i = 0; out != NULL && i + 6 <= len; i++) {
        reset |= memcmp(out + i, cmd0, 6) == 0;
    }
    if (!reset || _count == CARDS_MAX) {
        return false;
    }
    // The one GPIO driven low that no display listens on: the card's chip select.
    int cs = -1;
    for (int pin = 0; pin < DESKTOP_PIN_COUNT; pin++) {
        if (_low(pin) && !spi_virtual_uses_pin(pin)) {
            if (cs >= 0) {
                return false;   // more than one: which is the card's is not known
            }
            cs = pin;
        }
    }
    const char *why;
    if (cs < 0 || !sdcard_virtual_add(cs, &why)) {
        return false;
    }
    char name[24];
    parts_pin_name((uint8_t)cs, name, sizeof(name));
    fprintf(stderr, "virtual: SD card attached at %s (automatic; [spi] SD = \"sdcard cs=%s\")\r\n", name, name);
    return true;
}
