// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>

// A playbook (--playbook FILE): panel lines that drive a run of code.py, and four of its own:
// "sleep MS", "wait "TEXT" [MS]", "expect "TEXT"", "expect-not "TEXT"". It plays on the VM
// thread and the program's clock, from its top at each code.py run; the faults it sets end with
// the run. Its outcome joins the run's result (after_run.h).

// Read at startup; false (said on stderr) when the file cannot be read.
bool playbook_load(const char *path);
// code.py is about to run: from the top.
void playbook_start(void);
// What the program printed, as the console shows it.
void playbook_output(const char *text, size_t len);
// From the background task: a sleep or a wait's limit runs out.
void playbook_task(void);
// The run ended: false when no playbook played in it; else its status ("pass" or "fail") and
// why it failed, and it stops.
bool playbook_run_ended(const char **status, const char **detail);
