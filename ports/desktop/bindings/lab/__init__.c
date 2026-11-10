// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "after_run.h"
#include "api_ring.h"
#include "audio_capture.h"
#include "board_parts.h"
#include "bus_trace.h"
#include "control_api.h"
#include "desktop_port.h"
#include "fault.h"
#include "heap_walk.h"
#include "panel.h"

//| """The lab, from the program
//|
//| What the lab does from outside (the control API, ``cpapi``, the MCP server), the program
//| does here: parts on the board, panel lines, its run's result. There is no ``lab`` on a board,
//| so code that runs on both imports it in a ``try``."""
//|
//| parts: object
//| """The catalogue of parts: ``lab.parts.LED(board.D5)`` describes one, `attach` puts it on."""
//|
//| attached: dict
//| """Every part on the board, name to part object, made anew on each read."""
//|
//|

//| def attach(part: object) -> object:
//|     """Puts a part on the virtual board and returns it. RuntimeError on an adapter's board."""
//|     ...
//|
//|

//| def detach(part: object) -> None:
//|     """Takes a part, or the part of that name, off the board."""
//|     ...
//|
//|

//| def panel(*lines: str) -> None:
//|     """Applies panel lines (``press A``, ``set A0 40000``, ``fault sd.write error once`` ...)
//|     in order, as the control API's ``/api/panel`` does; the faults set here end with the run.
//|     ValueError naming the first line that is not one."""
//|     ...
//|
//|
static mp_obj_t _panel(size_t n_args, const mp_obj_t *args) {
    for (size_t i = 0; i < n_args; i++) {
        const char *line = mp_obj_str_get_str(args[i]);
        char err[160];
        if (!panel_apply_inside(line, err, sizeof(err))) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("%s"), err);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(_panel_obj, 0, _panel);

//| def fired() -> list[tuple[str, str, int]]:
//|     """The faults fired in this run: ``(point, action, times)`` each."""
//|     ...
//|
//|
static mp_obj_t _fired(void) {
    mp_obj_t list = mp_obj_new_list(0, NULL);
    const char *point, *action;
    unsigned times;
    for (size_t i = 0; fault_fired(i, &point, &action, &times); i++) {
        mp_obj_t t[3] = { mp_obj_new_str_from_cstr(point), mp_obj_new_str_from_cstr(action), mp_obj_new_int(times) };
        mp_obj_list_append(list, mp_obj_new_tuple(3, t));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(_fired_obj, _fired);

//| def result(status: str, detail: str = "") -> None:
//|     """Reports the run's result: ``"pass"``, ``"fail"`` or ``"skip"``, with a detail. A later
//|     call replaces it. ``cpapi`` and the after-run report give it once the run has ended; a run
//|     that ends in an exception or a hard fault has failed whatever it reported."""
//|     ...
//|
//|
static mp_obj_t _result(size_t n_args, const mp_obj_t *args) {
    const char *status = mp_obj_str_get_str(args[0]);
    if (strcmp(status, "pass") != 0 && strcmp(status, "fail") != 0 && strcmp(status, "skip") != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("status is \"pass\", \"fail\" or \"skip\""));
    }
    after_run_report(status, n_args > 1 ? mp_obj_str_get_str(args[1]) : "");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(_result_obj, 1, 2, _result);

//| def exit(status: int = 0) -> None:
//|     """Ends the lab's process with this exit status."""
//|     ...
//|
//|
static mp_obj_t _lab_exit(size_t n_args, const mp_obj_t *args) {
    desktop_power_off(n_args > 0 ? mp_obj_get_int(args[0]) : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(_exit_obj, 0, 1, _lab_exit);

// What the control API answers, read as json.loads reads it: the program and cpapi get the same.
static mp_obj_t _as_api(void (*answer)(api_buf_t *)) {
    api_buf_t b = {0};
    answer(&b);
    if (b.failed) {
        api_buf_free(&b);
        mp_raise_msg(&mp_type_MemoryError, NULL);
    }
    mp_obj_t text = mp_obj_new_str(b.data, b.len);
    api_buf_free(&b);
    mp_obj_t json = mp_import_name(MP_QSTR_json, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    return mp_call_function_1(mp_load_attr(json, MP_QSTR_loads), text);
}

//| def trace(since: int = 0) -> tuple[str, int]:
//|     """The bus trace's lines from offset since, and the offset to read on from. The lines are
//|     kept from the first call (or from the start with ``--api``): call it once early for an
//|     offset, then again with it."""
//|     ...
//|
//|
static mp_obj_t _trace(size_t n_args, const mp_obj_t *args) {
    mp_int_t since = n_args > 0 ? mp_obj_get_int(args[0]) : 0;
    if (!bus_trace_keep()) {
        mp_raise_msg(&mp_type_MemoryError, NULL);
    }
    char *text = NULL;
    api_ring_read_t info;
    size_t n = api_ring_read(bus_trace_kept(), since < 0 ? 0 : (uint64_t)since, NULL, 0, SIZE_MAX, &text, &info);
    // A character the ring's start cut in two is left out.
    size_t cut = 0;
    while (text != NULL && cut < n && ((unsigned char)text[cut] & 0xc0) == 0x80) {
        cut++;
    }
    mp_obj_t got[2] = { mp_obj_new_str(text != NULL ? text + cut : "", n - cut), mp_obj_new_int_from_ull(info.next) };
    free(text);
    return mp_obj_new_tuple(2, got);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(_trace_obj, 0, 1, _trace);

//| def claimed() -> list[str]:
//|     """The pins claimed now, by the board's names (``cpapi``'s ``claimed()``)."""
//|     ...
//|
//|
static mp_obj_t _claimed(void) {
    return _as_api(after_run_claimed_json);
}
static MP_DEFINE_CONST_FUN_OBJ_0(_claimed_obj, _claimed);

//| def audio() -> list[dict]:
//|     """What each audio output played since recording started (``--audio-capture``): pin, rate,
//|     channels, frames, dropped, underruns, and the first channel's peak and rms (0 to 1),
//|     frequency (Hz, None in silence) and silences (20 ms or more)."""
//|     ...
//|
//|
static mp_obj_t _audio(void) {
    return mp_obj_subscr(_as_api(audio_capture_json), MP_OBJ_NEW_QSTR(MP_QSTR_recordings), MP_OBJ_SENTINEL);
}
static MP_DEFINE_CONST_FUN_OBJ_0(_audio_obj, _audio);

//| def heap() -> dict:
//|     """The heap by type: total, used, free, largest_free, and types: ``{name: [count, bytes]}``,
//|     the largest first (``bytes`` for blocks that are no object: buffers, lists' items).
//|     Collect first (``gc.collect()``) to count only what is alive."""
//|     ...
//|
//|
static mp_obj_t _heap(void) {
    return _as_api(heap_json);
}
static MP_DEFINE_CONST_FUN_OBJ_0(_heap_obj, _heap);

//| def info() -> dict:
//|     """The lab: version, machine, board, board_file, profile, heap_limit, sockets, drive,
//|     fast_time, skipped_ms, pid, as ``cpapi``'s ``info()``."""
//|     ...
//|
//|
static mp_obj_t _info(void) {
    return _as_api(control_api_info_json);
}
static MP_DEFINE_CONST_FUN_OBJ_0(_info_obj, _info);

static const mp_rom_map_elem_t lab_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_lab) },
    { MP_ROM_QSTR(MP_QSTR_parts), MP_ROM_PTR(&lab_parts_module) },
    { MP_ROM_QSTR(MP_QSTR_attach), MP_ROM_PTR(&lab_attach_obj) },
    { MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&lab_detach_obj) },
    { MP_ROM_QSTR(MP_QSTR_panel), MP_ROM_PTR(&_panel_obj) },
    { MP_ROM_QSTR(MP_QSTR_fired), MP_ROM_PTR(&_fired_obj) },
    { MP_ROM_QSTR(MP_QSTR_result), MP_ROM_PTR(&_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_exit), MP_ROM_PTR(&_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_trace), MP_ROM_PTR(&_trace_obj) },
    { MP_ROM_QSTR(MP_QSTR_claimed), MP_ROM_PTR(&_claimed_obj) },
    { MP_ROM_QSTR(MP_QSTR_audio), MP_ROM_PTR(&_audio_obj) },
    { MP_ROM_QSTR(MP_QSTR_heap), MP_ROM_PTR(&_heap_obj) },
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&_info_obj) },
    // lab.attached
    { MP_ROM_QSTR(MP_QSTR___getattr__), MP_ROM_PTR(&lab_getattr_obj) },
};
static MP_DEFINE_CONST_DICT(lab_module_globals, lab_module_globals_table);

const mp_obj_module_t lab_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&lab_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lab, lab_module);
