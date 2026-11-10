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

// The points, what can happen at each, and the filters each takes: a text one and a number one.
static const struct {
    const char *point;
    const char *actions[4];
    const char *text, *number;
} _points[] = {
    { "sd.read", { "error", "timeout" }, NULL, "block" },
    { "sd.write", { "error", "timeout" }, NULL, "block" },
    { "i2c", { "nack", "hold" }, NULL, "addr" },
    { "wifi.connect", { "fail", "auth", "noap" }, "ssid", NULL },
    { "wifi.link", { "drop" }, NULL, NULL },
    { "net.dns", { "fail" }, "host", NULL },
    { "net.socket", { "full" }, NULL, NULL },
    { "net.connect", { "refuse", "timeout" }, "host", "port" },
    { "net.send", { "error", "cut" }, "host", "port" },
    { "net.recv", { "reset", "cut", "stall" }, "host", "port" },
};

typedef struct {
    bool used;
    bool inside;
    int point;              // in _points
    const char *action;
    long arg;
    char text[64];          // the filters: "" and -1 for any
    long number;
    long after;             // calls let through first
    long count;             // firings, -1: every time
    long hits, fires;
    char line[96];          // the line, for the report
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

// Where the call was, for the trace: "block 7", "0x18", "example.com:80", "Lab".
static void _where(char *out, size_t len, int p, const char *text, long number) {
    const char *name = _points[p].number;
    if (text != NULL && number >= 0) {
        snprintf(out, len, " %s:%ld", text, number);
    } else if (text != NULL) {
        snprintf(out, len, " %s", text);
    } else if (number >= 0 && name != NULL && strcmp(name, "addr") == 0) {
        snprintf(out, len, " 0x%02lx", number);
    } else if (number >= 0 && name != NULL) {
        snprintf(out, len, " %s %ld", name, number);
    } else {
        out[0] = '\0';
    }
}

fault_t fault_check(const char *point, const char *text, long number) {
    int p = _point(point);
    for (size_t i = 0; p >= 0 && i < RULES; i++) {
        rule_t *r = &_rules[i];
        if (!r->used || r->point != p || (r->text[0] != '\0' && (text == NULL || strcmp(r->text, text) != 0))
            || (r->number >= 0 && r->number != number)) {
            continue;
        }
        r->hits++;
        if (r->hits <= r->after || (r->count >= 0 && r->fires >= r->count)) {
            continue;
        }
        r->fires++;
        _note_fired(_points[p].point, r->action);
        char where[80];
        _where(where, sizeof(where), p, text, number);
        bus_trace_text("FAULT %s%s: %s", point, where, r->action);
        return (fault_t) { true, r->action, r->arg };
    }
    return (fault_t) { false, NULL, 0 };
}

// "clear [POINT]": inside, the run's own rules; outside, all.
static bool _clear(const char *what, bool inside, char *err, size_t errlen) {
    int p = what[0] != '\0' ? _point(what) : -1;
    if (what[0] != '\0' && p < 0) {
        snprintf(err, errlen, "no fault point %s", what);
        return false;
    }
    for (size_t i = 0; i < RULES; i++) {
        rule_t *r = &_rules[i];
        if (r->used && (!inside || r->inside) && (p < 0 || r->point == p)) {
            r->used = false;
        }
    }
    return true;
}

// "a fault on net.dns takes host=, after=, count= or once"
static void _filters_error(char *err, size_t errlen, int p, const char *given) {
    int len = snprintf(err, errlen, "a fault on %s takes ", _points[p].point);
    for (int k = 0; k < 2; k++) {
        const char *name = k ? _points[p].number : _points[p].text;
        if (name != NULL && len < (int)errlen) {
            len += snprintf(err + len, errlen - len, "%s=, ", name);
        }
    }
    if (len < (int)errlen) {
        snprintf(err + len, errlen - len, "after=, count= or once, not %s=", given);
    }
}

static bool _number(const char *text, long *out) {
    char *end;
    *out = strtol(text, &end, 0);
    return end != text && *end == '\0' && *out >= 0;
}

bool fault_line(const char *line, bool inside, char *err, size_t errlen) {
    char words[10][64];
    int n = sscanf(line, "%*s %63s %63s %63s %63s %63s %63s %63s %63s %63s %63s", words[0], words[1], words[2],
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
    rule_t r = { .used = true, .inside = inside, .point = p, .number = -1, .count = -1 };
    for (size_t a = 0; n >= 2 && a < 4 && _points[p].actions[a] != NULL; a++) {
        if (strcmp(words[1], _points[p].actions[a]) == 0) {
            r.action = _points[p].actions[a];
        }
    }
    if (r.action == NULL) {
        int len = snprintf(err, errlen, "%s does:", _points[p].point);
        for (size_t a = 0; a < 4 && _points[p].actions[a] != NULL && len < (int)errlen; a++) {
            len += snprintf(err + len, errlen - len, "%s %s", a == 0 ? "" : " or", _points[p].actions[a]);
        }
        return false;
    }
    for (int w = 2; w < n; w++) {
        char *eq = strchr(words[w], '=');
        long v;
        if (strcmp(words[w], "once") == 0) {
            r.count = 1;
        } else if (eq == NULL && w == 2 && _number(words[w], &v)) {
            r.arg = v;
        } else if (eq != NULL) {
            *eq = '\0';
            const char *value = eq + 1;
            bool text = _points[p].text != NULL && strcmp(words[w], _points[p].text) == 0;
            long *field = strcmp(words[w], "after") == 0 ? &r.after
                : strcmp(words[w], "count") == 0 ? &r.count
                : _points[p].number != NULL && strcmp(words[w], _points[p].number) == 0 ? &r.number : NULL;
            if (text && value[0] != '\0') {
                snprintf(r.text, sizeof(r.text), "%s", value);
            } else if (field != NULL && _number(value, &v)) {
                *field = v;
            } else if (field == NULL && !text) {
                _filters_error(err, errlen, p, words[w]);
                return false;
            } else {
                snprintf(err, errlen, "a fault's %s=%s is not %s", words[w], value, text ? "a name" : "a number");
                return false;
            }
        } else {
            snprintf(err, errlen, "a fault's %s is not a number or a filter", words[w]);
            return false;
        }
    }
    snprintf(r.line, sizeof(r.line), "%s", line + strspn(line, " \t"));
    r.line[strcspn(r.line, "\r\n")] = '\0';
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
            api_buf_json_string(b, r->line, strlen(r->line));
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
