// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "api_json.h"

// Faults: named points in the virtual devices and the network where a call can fail, on a count.
// A rule is a panel line, "fault POINT ACTION [ARG] [FILTER=VALUE ...] [after=N] [count=N|once]";
// the first rule that matches a call decides. All on the VM thread.

typedef struct {
    bool fired;
    const char *action;     // the rule's action, one of the point's
    long arg;
} fault_t;

// The rule that decides this call of the point, if one fires: counted, and traced as
// "FAULT point [where]: action". What the call is about, for the point's filters: text (a host,
// an SSID; NULL for none) and number (a block, an address, a port; -1 for none).
fault_t fault_check(const char *point, const char *text, long number);

// A "fault ..." line. inside: from the run (the program, a playbook), its rules end with it;
// else from outside (the API, the key script), they stay. False with why in err.
bool fault_line(const char *line, bool inside, char *err, size_t errlen);

// At the end of a VM: the run's rules go, what fired is kept for the after-run report.
void fault_run_ended(void);

// The faults fired in this run: (point, action, times) each, in order; false past the last.
bool fault_fired(size_t i, const char **point, const char **action, unsigned *times);

// {"rules": [{"rule", "inside", "hits", "fired"}], "fired": [...]}: now.
void fault_json(api_buf_t *b);
// [{"point", "action", "times"}] of the last run that ended.
void fault_last_run_json(api_buf_t *b);
