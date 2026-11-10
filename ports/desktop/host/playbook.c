// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "playbook.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus_trace.h"
#include "desktop_port.h"
#include "panel.h"

#define LINES 256
#define LINE_MAX 256
#define OUTPUT 16384            // the output since the last wait's match that is kept
#define WAIT_MS 10000           // a wait's limit when it names none

typedef enum {
    IDLE,       // no code.py run, or no playbook
    RUNNING,
    SLEEPING,
    WAITING,
    PASSED,
    FAILED,
} state_t;

static char *_lines[LINES];
static int _count;
static state_t _state;
static int _pc;                 // the line playing
static uint64_t _until;         // a sleep's end, a wait's limit, in program ns
static char _text[LINE_MAX];    // what the wait waits for
static char _why[300];
static char _out[OUTPUT + 1];   // the output since the last wait's match
static size_t _out_len;
static bool _stepping;

bool playbook_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "circuitpython: cannot read the playbook %s\r\n", path);
        return false;
    }
    char line[LINE_MAX];
    while (_count < LINES && fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        _lines[_count++] = strdup(line);
    }
    fclose(f);
    return true;
}

__attribute__((format(printf, 1, 2))) static void _fail(const char *fmt, ...) {
    int n = snprintf(_why, sizeof(_why), "playbook line %d: ", _pc + 1);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(_why + n, sizeof(_why) - n, fmt, ap);
    va_end(ap);
    _state = FAILED;
    bus_trace_text("PLAYBOOK %s", _why);
}

// "TEXT" at s, into out; the rest after its closing quote. False when it is not quoted.
static bool _quoted(const char *s, char *out, size_t cap, const char **rest) {
    s += strspn(s, " \t");
    const char *end = *s == '"' ? strrchr(s + 1, '"') : NULL;
    if (end == NULL || (size_t)(end - s - 1) >= cap) {
        return false;
    }
    memcpy(out, s + 1, end - s - 1);
    out[end - s - 1] = '\0';
    *rest = end + 1;
    return true;
}

// True when text is in the output kept; the output then starts after it.
static bool _seen(const char *text) {
    char *at = strstr(_out, text);
    if (at == NULL) {
        return false;
    }
    size_t past = at - _out + strlen(text);
    memmove(_out, _out + past, _out_len - past + 1);
    _out_len -= past;
    return true;
}

static void _step(void) {
    if (_stepping) {
        return;     // a line that prints plays no further lines from the console's write
    }
    _stepping = true;
    while (_state == RUNNING && _pc < _count) {
        const char *line = _lines[_pc] + strspn(_lines[_pc], " \t");
        const char *rest;
        char text[LINE_MAX];
        long ms;
        if (*line == '\0' || *line == '#') {
            _pc++;
        } else if (sscanf(line, "sleep %ld", &ms) == 1) {
            _until = desktop_program_ns() + (uint64_t)ms * 1000000;
            _state = SLEEPING;
            desktop_wake_at(_until);
        } else if (strncmp(line, "wait ", 5) == 0) {
            if (!_quoted(line + 5, text, sizeof(text), &rest)) {
                _fail("wait needs a \"TEXT\"");
            } else if (_seen(text)) {
                _pc++;
            } else {
                ms = sscanf(rest, "%ld", &ms) == 1 ? ms : WAIT_MS;
                strcpy(_text, text);
                _until = desktop_program_ns() + (uint64_t)ms * 1000000;
                _state = WAITING;
                desktop_wake_at(_until);
            }
        } else if (strncmp(line, "expect ", 7) == 0 || strncmp(line, "expect-not ", 11) == 0) {
            bool negated = line[6] == '-';
            if (!_quoted(line + (negated ? 11 : 7), text, sizeof(text), &rest)) {
                _fail("%s needs a \"TEXT\"", negated ? "expect-not" : "expect");
            } else if ((strstr(_out, text) != NULL) == negated) {
                _fail(negated ? "\"%s\" was printed" : "\"%s\" was not printed", text);
            } else {
                _pc++;
            }
        } else {
            char err[200];
            if (panel_apply_inside(line, err, sizeof(err))) {
                _pc++;
            } else {
                _fail("%s", err);
            }
        }
    }
    if (_state == RUNNING) {
        _state = PASSED;
        bus_trace_text("PLAYBOOK passed");
    }
    _stepping = false;
}

void playbook_start(void) {
    if (_count == 0) {
        return;
    }
    _pc = 0;
    _out_len = 0;
    _out[0] = '\0';
    _why[0] = '\0';
    _state = RUNNING;
    _step();
}

void playbook_output(const char *text, size_t len) {
    if (_state != RUNNING && _state != SLEEPING && _state != WAITING) {
        return;
    }
    if (len > OUTPUT) {
        text += len - OUTPUT;
        len = OUTPUT;
    }
    if (_out_len + len > OUTPUT) {
        // The oldest goes: a wait looks for what comes, not for what came long ago.
        size_t drop = _out_len + len - OUTPUT;
        memmove(_out, _out + drop, _out_len - drop);
        _out_len -= drop;
    }
    memcpy(_out + _out_len, text, len);
    _out_len += len;
    _out[_out_len] = '\0';
    if (_state == WAITING && _seen(_text)) {
        _state = RUNNING;
        _pc++;
        _step();
    }
}

void playbook_task(void) {
    if ((_state != SLEEPING && _state != WAITING) || desktop_program_ns() < _until) {
        return;
    }
    if (_state == WAITING) {
        _fail("\"%s\" did not come", _text);
        return;
    }
    _state = RUNNING;
    _pc++;
    _step();
}

bool playbook_run_ended(const char **status, const char **detail) {
    state_t state = _state;
    if (state == IDLE) {
        return false;
    }
    _state = IDLE;
    if (state == RUNNING || state == SLEEPING || state == WAITING) {
        snprintf(_why, sizeof(_why), "playbook line %d: the run ended %s", _pc + 1,
            state == WAITING ? "while it waited" : "before the playbook did");
        if (state == WAITING) {
            snprintf(_why + strlen(_why), sizeof(_why) - strlen(_why), " for \"%s\"", _text);
        }
        state = FAILED;
    }
    *status = state == PASSED ? "pass" : "fail";
    *detail = state == PASSED ? "" : _why;
    return true;
}
