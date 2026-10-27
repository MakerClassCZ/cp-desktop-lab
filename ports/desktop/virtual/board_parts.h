// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "parts.h"

// Parts from Python on the virtual board: board.parts is the catalogue of kinds, a kind called
// describes a part, board.attach puts it on the board, and an attached part object is a handle
// to the live part (parts.c, i2c_virtual.c, neopixel_emu.c), found by its place on each access.

typedef enum {
    BOARD_PART_PIN,         // a part_t on a pin
    BOARD_PART_I2C,         // a device on the virtual I2C bus
    BOARD_PART_NEOPIXEL,    // a strip on its data pin
} board_part_category_t;

typedef struct {
    mp_obj_base_t base;
    qstr name;
    uint8_t category;
    part_kind_t kind;
    qstr keyword;           // the one option keyword, or MP_QSTRnull
    const char *initial;    // its default, as the board file writes it
    const char *fixed;      // an option string the kind always has: "servo"
    const char *model;      // I2C: "bme280"
} board_part_kind_t;

typedef struct {
    mp_obj_base_t base;
    const board_part_kind_t *kind;
    uint8_t pin;
    uint8_t address;
    bool attached;
    char name[24];
    char options[64];
} board_part_obj_t;

extern const mp_obj_module_t board_parts_module;
MP_DECLARE_CONST_FUN_OBJ_1(board_attach_obj);
MP_DECLARE_CONST_FUN_OBJ_1(board_detach_obj);
