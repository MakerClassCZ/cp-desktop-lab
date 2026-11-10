// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "fault.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus_trace.h"

#define RULES 32
#define FIRED 16

// The points and what can happen at each.
static const struct {
    const char *point;
    const char *actions[3];
} _points[] = {
    { "sd.read", { "error", "timeout" } },
    { "sd.write", { "error", "timeout" } },
    { "i2c", { "nack", "hold" } },
};

typedef struct {
    bool used;
    bool inside;
    const char *point;      // from _points
    const char *action;
    long arg;
    long block, addr;       // filters, -1: any
    long after;             // calls let through first
    long count;             // firings, -1: every time
    long hits, fires;
    char text[96];          // the line, for the report
} rule_t;

typedef struct {
    const char *point, *action;
    unsigned times;
} fired_t;

static rule_t _rules[RULES];
static fired_t _fired[FIRED], _last_fired[FIRED];

static int _point(const char *name) {
    for (size_t i = 0; i < sizeof(_points) / sizeof(_points[0]); i++) {
        if (strcmp(_points[i].point, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void _note_fired(const char *point, const char *action) {
    for (size_t i = 0; i < FIRED; i++) {
        fired_t *f = &_fired[i];
        if (f->point == NULL) {
            *f = (fired_t) { point, action, 0 };
        }
        if (f->point == point && f->action == action) {
            f->times++;
            return;
        }
    }
}

fault_t fault_check(const char *point, const fault_ctx_t *ctx) {
    for (size_t i = 0; i < RULES; i++) {
        rule_t *r = &_rules[i];
        if (!r->used || strcmp(r->point, point) != 0 || (r->block >= 0 && r->block != ctx->block)
            || (r->addr >= 0 && r->addr != ctx->addr)) {
            continue;
        }
        r->hits++;
        if (r->hits <= r->after || (r->count >= 0 && r->fires >= r->count)) {
            continue;
        }
        r->fires++;
        _note_fired(r->point, r->action);
        if (ctx->block >= 0) {
            bus_trace_text("FAULT %s block %ld: %s", point, ctx->block, r->action);
        } else if (ctx->addr >= 0) {
            bus_trace_text("FAULT %s 0x%02lx: %s", point, ctx->addr, r->action);
        } else {
            bus_trace_text("FAULT %s: %s", point, r->action);
        }
        return (fault_t) { true, r->action, r->arg };
    }
    return (fault_t) { false, NULL, 0 };
}

// "clear [POINT]": inside, the run's own rules; outside, all.
static bool _clear(const char *what, bool inside, char *err, size_t errlen) {
    if (what[0] != '\0' && _point(what) < 0) {
        snprintf(err, errlen, "no fault point %s", what);
        return false;
    }
    for (size_t i = 0; i < RULES; i++) {
        rule_t *r = &_rules[i];
        if (r->used && (!inside || r->inside) && (what[0] == '\0' || strcmp(r->point, what) == 0)) {
            r->used = false;
        }
    }
    return true;
}

static bool _number(const char *text, long *out) {
    char *end;
    *out = strtol(text, &end, 0);
    return end != text && *end == '\0' && *out >= 0;
}

bool fault_line(const char *line, bool inside, char *err, size_t errlen) {
    char words[10][40];
    int n = sscanf(line, "%*s %39s %39s %39s %39s %39s %39s %39s %39s %39s %39s", words[0], words[1], words[2],
        words[3], words[4], words[5], words[6], words[7], words[8], words[9]);
    if (n >= 1 && strcmp(words[0], "clear") == 0) {
        return _clear(n >= 2 ? words[1] : "", inside, err, errlen);
    }
    int p = n >= 1 ? _point(words[0]) : -1;
    if (p < 0) {
        int len = snprintf(err, errlen, "fault needs a point:");
        for (size_t i = 0; i < sizeof(_points) / sizeof(_points[0]) && len < (int)errlen; i++) {
            len += snprintf(err + len, errlen - len, " %s", _points[i].point);
        }
        return false;
    }
    rule_t r = { .used = true, .inside = inside, .point = _points[p].point, .block = -1, .addr = -1, .count = -1 };
    for (size_t a = 0; n >= 2 && a < 3 && _points[p].actions[a] != NULL; a++) {
        if (strcmp(words[1], _points[p].actions[a]) == 0) {
            r.action = _points[p].actions[a];
        }
    }
    if (r.action == NULL) {
        snprintf(err, errlen, "%s does: %s%s%s", r.point, _points[p].actions[0], _points[p].actions[1] ? " or " : "",
            _points[p].actions[1] ? _points[p].actions[1] : "");
        return false;
    }
    for (int w = 2; w < n; w++) {
        char *eq = strchr(words[w], '=');
        long v;
        if (strcmp(words[w], "once") == 0) {
            r.count = 1;
        } else if (eq == NULL && w == 2 && _number(words[w], &v)) {
            r.arg = v;
        } else if (eq != NULL && _number(eq + 1, &v)) {
            *eq = '\0';
            long *field = strcmp(words[w], "after") == 0 ? &r.after
                : strcmp(words[w], "count") == 0 ? &r.count
                : strcmp(words[w], "block") == 0 ? &r.block
                : strcmp(words[w], "addr") == 0 ? &r.addr : NULL;
            if (field == NULL) {
                snprintf(err, errlen, "a fault takes after=, count=, once, block= or addr=, not %s=", words[w]);
                return false;
            }
            *field = v;
        } else {
            snprintf(err, errlen, "a fault's %s is not a number or a filter", words[w]);
            return false;
        }
    }
    snprintf(r.text, sizeof(r.text), "%s", line + strspn(line, " \t"));
    r.text[strcspn(r.text, "\r\n")] = '\0';
    for (size_t i = 0; i < RULES; i++) {
        if (!_rules[i].used) {
            _rules[i] = r;
            return true;
        }
    }
    snprintf(err, errlen, "more than %d fault rules", RULES);
    return false;
}

void fault_run_ended(void) {
    for (size_t i = 0; i < RULES; i++) {
        if (_rules[i].inside) {
            _rules[i].used = false;
        }
    }
    memcpy(_last_fired, _fired, sizeof(_fired));
    memset(_fired, 0, sizeof(_fired));
}

bool fault_fired(size_t i, const char **point, const char **action, unsigned *times) {
    if (i >= FIRED || _fired[i].point == NULL) {
        return false;
    }
    *point = _fired[i].point;
    *action = _fired[i].action;
    *times = _fired[i].times;
    return true;
}

static void _fired_json(api_buf_t *b, const fired_t *fired) {
    api_buf_add(b, "[", 1);
    for (size_t i = 0; i < FIRED && fired[i].point != NULL; i++) {
        api_buf_printf(b, "%s{\"point\": \"%s\", \"action\": \"%s\", \"times\": %u}", i ? ", " : "", fired[i].point,
            fired[i].action, fired[i].times);
    }
    api_buf_add(b, "]", 1);
}

void fault_json(api_buf_t *b) {
    api_buf_add(b, "{\"rules\": [", 11);
    bool first = true;
    for (size_t i = 0; i < RULES; i++) {
        rule_t *r = &_rules[i];
        if (r->used) {
            api_buf_add(b, first ? "{\"rule\": " : ", {\"rule\": ", first ? 9 : 11);
            api_buf_json_string(b, r->text, strlen(r->text));
            api_buf_printf(b, ", \"inside\": %s, \"hits\": %ld, \"fired\": %ld}", r->inside ? "true" : "false", r->hits,
                r->fires);
            first = false;
        }
    }
    api_buf_add(b, "], \"fired\": ", 12);
    _fired_json(b, _fired);
    api_buf_add(b, "}", 1);
}

void fault_last_run_json(api_buf_t *b) {
    _fired_json(b, _last_fired);
}
