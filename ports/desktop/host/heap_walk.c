// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "heap_walk.h"

#include <stdlib.h>
#include <string.h>

#include "py/gc.h"
#include "py/mpstate.h"
#include "py/obj.h"
#include "py/objmodule.h"
#include "py/qstr.h"

// py/gc.c's allocation table: two bits a block, four blocks a byte.
#define BLOCK_BYTES (MICROPY_BYTES_PER_GC_BLOCK)
#define KIND(area, block) (((area)->gc_alloc_table_start[(block) / 4] >> (2 * ((block) & 3))) & 3)
#define AT_FREE 0
#define AT_TAIL 2

#define NAMES 256

// Types the core makes objects of that no module names.
extern const mp_obj_type_t mp_type_closure, mp_type_cell;
static const mp_obj_type_t *const _core[] = {
    &mp_type_type, &mp_type_object, &mp_type_fun_bc, &mp_type_closure, &mp_type_cell, &mp_type_bound_meth,
    &mp_type_gen_instance, &mp_type_code, &mp_type_module, &mp_type_staticmethod, &mp_type_classmethod,
    &mp_type_property, &mp_type_memoryview, &mp_type_array,
};

typedef struct {
    const void **items;
    size_t len, cap;
} types_t;

static void _add(types_t *t, mp_obj_t o) {
    if (!mp_obj_is_type(o, &mp_type_type)) {
        return;
    }
    if (t->len == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 512;
        const void **items = realloc(t->items, cap * sizeof(*items));
        if (items == NULL) {
            return;
        }
        t->items = items;
        t->cap = cap;
    }
    t->items[t->len++] = MP_OBJ_TO_PTR(o);
}

// The types a map names, and those inside them (countio.Edge in countio.Counter's module).
static void _from_map(types_t *t, const mp_map_t *map, int depth) {
    for (size_t i = 0; i < map->alloc; i++) {
        mp_obj_t v = map->table[i].value;
        if (!mp_map_slot_is_filled(map, i) || !mp_obj_is_obj(v)) {
            continue;
        }
        _add(t, v);
        if (depth > 0 && mp_obj_is_type(v, &mp_type_module)) {
            _from_map(t, &((mp_obj_module_t *)MP_OBJ_TO_PTR(v))->globals->map, depth - 1);
        } else if (depth > 0 && mp_obj_is_type(v, &mp_type_type)) {
            const mp_obj_type_t *type = MP_OBJ_TO_PTR(v);
            mp_obj_dict_t *locals = MP_OBJ_TYPE_GET_SLOT_OR_NULL(type, locals_dict);
            if (locals != NULL) {
                _from_map(t, &locals->map, depth - 1);
            }
        }
    }
}

static int _compare(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)*(const void *const *)a, y = (uintptr_t)*(const void *const *)b;
    return x < y ? -1 : x > y;
}

static bool _known(const types_t *t, const void *p) {
    return bsearch(&p, t->items, t->len, sizeof(*t->items), _compare) != NULL;
}

// The block a pointer starts, in one of the heap's areas; NULL when it starts none.
static const mp_state_mem_area_t *_area_of(const void *p) {
    for (const mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = area->next) {
        const byte *b = p;
        if (b >= area->gc_pool_start && b < area->gc_pool_end && (size_t)(b - area->gc_pool_start) % BLOCK_BYTES == 0) {
            size_t block = (size_t)(b - area->gc_pool_start) / BLOCK_BYTES;
            return KIND(area, block) != AT_FREE && KIND(area, block) != AT_TAIL ? area : NULL;
        }
    }
    return NULL;
}

// A block's type name: a known type, or a class on the heap (whose own first word is
// mp_type_type); 0 for neither.
static qstr _type_name(const types_t *t, const void *block) {
    const void *first = *(const void *const *)block;
    if (_known(t, first)) {
        return ((const mp_obj_type_t *)first)->name;
    }
    if (_area_of(first) != NULL && *(const void *const *)first == &mp_type_type) {
        return ((const mp_obj_type_t *)first)->name;
    }
    return 0;
}

typedef struct {
    qstr name;              // 0: "bytes"
    size_t count, bytes;
} tally_t;

static int _larger(const void *a, const void *b) {
    const tally_t *x = a, *y = b;
    return x->bytes < y->bytes ? 1 : x->bytes > y->bytes ? -1 : 0;
}

void heap_json(api_buf_t *b) {
    if (!gc_alloc_possible()) {
        api_buf_add(b, "{}", 2);
        return;
    }
    types_t t = {0};
    for (size_t i = 0; i < MP_ARRAY_SIZE(_core); i++) {
        _add(&t, MP_OBJ_FROM_PTR(_core[i]));
    }
    _from_map(&t, &mp_builtin_module_map, 2);
    _from_map(&t, &MP_STATE_VM(mp_loaded_modules_dict).map, 2);
    qsort(t.items, t.len, sizeof(*t.items), _compare);

    static tally_t tally[NAMES];
    size_t names = 0;
    for (const mp_state_mem_area_t *area = &MP_STATE_MEM(area); area != NULL; area = area->next) {
        size_t blocks = (size_t)(area->gc_pool_end - area->gc_pool_start) / BLOCK_BYTES;
        for (size_t block = 0; block < blocks; block++) {
            int kind = KIND(area, block);
            if (kind == AT_FREE || kind == AT_TAIL) {
                continue;
            }
            size_t length = 1;
            while (block + length < blocks && KIND(area, block + length) == AT_TAIL) {
                length++;
            }
            qstr name = _type_name(&t, area->gc_pool_start + block * BLOCK_BYTES);
            size_t k = 0;
            while (k < names && tally[k].name != name) {
                k++;
            }
            if (k == names && names < NAMES) {
                tally[names++] = (tally_t) { name, 0, 0 };
            } else if (k == names) {
                k = 0;      // past NAMES kinds: counted with the first
            }
            tally[k].count++;
            tally[k].bytes += length * BLOCK_BYTES;
            block += length - 1;
        }
    }
    free(t.items);
    qsort(tally, names, sizeof(tally[0]), _larger);

    gc_info_t info;
    gc_info(&info);
    api_buf_printf(b, "{\"total\": %lu, \"used\": %lu, \"free\": %lu, \"largest_free\": %lu, \"types\": {",
        (unsigned long)info.total, (unsigned long)info.used, (unsigned long)info.free,
        (unsigned long)(info.max_free * BLOCK_BYTES));    // gc_info gives this one in blocks
    for (size_t k = 0; k < names; k++) {
        const char *name = tally[k].name != 0 ? qstr_str(tally[k].name) : "bytes";
        name = name[0] != '\0' ? name : "unnamed";     // the core's cells have no name
        api_buf_add(b, k ? ", " : "", k ? 2 : 0);
        api_buf_json_string(b, name, strlen(name));
        api_buf_printf(b, ": [%lu, %lu]", (unsigned long)tally[k].count, (unsigned long)tally[k].bytes);
    }
    api_buf_add(b, "}}", 2);
}
