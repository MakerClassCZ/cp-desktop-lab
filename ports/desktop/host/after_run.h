// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include "api_json.h"

// What a run left behind: the pins still claimed once its VM is gone and the memory outside the
// heap. The objects free their pins themselves, as on the boards' ports, so a pin no object
// released stays claimed into the next run, where a board says it is in use too.

// From port_background_task(): notices a VM starting and ending.
void after_run_task(void);
// The result the running program reports (lab.result): "pass", "fail" or "skip", and a detail.
void after_run_report(const char *status, const char *detail);
// {"vm_running": bool, "runs": N, "result": null or {"status", "detail"}, "faults_fired": [...],
// "leaked_pins": [...],
// "outside_heap": bytes, "outside_heap_change": bytes} of the last VM that ended; runs counts the VMs ended so far
// (boot.py, code.py and the REPL each one), vm_running says whether one runs now.
void after_run_json(api_buf_t *b);
// ["D5", ...]: the pins claimed now, by the board's names.
void after_run_claimed_json(api_buf_t *b);
