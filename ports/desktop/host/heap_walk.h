// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "api_json.h"

// The VM's heap by type, on the VM thread while it runs: {"total", "used", "free", "largest_free",
// "types": {name: [blocks' count, bytes]}}, the largest first. A block counts under its object's
// type when its first word is a type: one of the modules' (builtins, every built-in module, the
// types inside them, the core's own) or a class on the heap; else under "bytes" (buffers, the
// items of lists and dicts, strings' text, code). Meant for leaks and for comparing two programs:
// the sizes are this program's (64-bit objects are larger than a board's). Nothing ({}) when no
// VM runs.
void heap_json(api_buf_t *b);
