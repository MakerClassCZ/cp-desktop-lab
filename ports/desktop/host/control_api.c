// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "control_api.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
typedef SOCKET sock_t;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCKET (-1)
#define close_socket close
#endif

#include "genhdr/mpversion.h"
#include "shared-bindings/os/__init__.h"

#include "py/mpconfig.h"

#include "api_json.h"
#include "api_ring.h"
#include "bus_trace.h"
#include "desktop_port.h"
#include "png_write.h"
#if CIRCUITPY_SDL
#include "panel.h"
#include "sdl_host.h"
#include "vdisplay.h"
#endif

#define CONNECTIONS_MAX 8
#define HEAD_MAX 16384
#define BODY_MAX (4 * 1024 * 1024)
#define RING_SIZE (64 * 1024)
#define IO_TIMEOUT_MS 10000     // a connection's longest silence while it sends or reads
#define EXEC_MAX_MS 600000
#define WAIT_MAX_MS 60000

typedef struct {
    sock_t sock;
    char method[8];
    char path[256];             // without the query
    char query[512];
    char *body;
    size_t body_len;
} request_t;

static sock_t _listener = BAD_SOCKET;
static int _port;
static char _host[64];          // the address bound, as a Host header may name it
static char _token[65];
static char _boot[9];
static int _connections;
static pthread_mutex_t _lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t _slot_free = PTHREAD_COND_INITIALIZER;
static bool _on;
static api_ring_t _console;
static api_ring_t _trace;
// The supervisor's status, from the terminal title it writes: "code.py", "REPL", "Done" ...
static char _status[64] = "starting";

// ---- the console's output ----

// The title escape, ESC ] 0 ; ... ESC \, its middle field (between " | ") the status.
static void _title(const char *title) {
    const char *a = strstr(title, " | ");
    const char *b = a ? strstr(a + 3, " | ") : NULL;
    if (b != NULL) {
        pthread_mutex_lock(&_lock);
        size_t n = (size_t)(b - a - 3) < sizeof(_status) - 1 ? (size_t)(b - a - 3) : sizeof(_status) - 1;
        memcpy(_status, a + 3, n);
        _status[n] = '\0';
        pthread_mutex_unlock(&_lock);
    }
}

void control_api_trace(const char *line, size_t len) {
    if (_on) {
        api_ring_write(&_trace, line, len);
    }
}

void control_api_console(const char *text, size_t len) {
    if (!_on) {
        return;
    }
    api_ring_write(&_console, text, len);
    // The title may come in pieces; only the VM thread writes the console.
    static const char start[] = "\x1b]0;";
    static char title[160];
    static size_t matched, used;
    static bool in_title;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (!in_title) {
            matched = c == start[matched] ? matched + 1 : c == start[0] ? 1 : 0;
            if (matched == sizeof(start) - 1) {
                in_title = true;
                used = 0;
                matched = 0;
            }
        } else if (c == '\x1b' || c == '\a') {
            title[used] = '\0';
            _title(title);
            in_title = false;
        } else if (used < sizeof(title) - 1) {
            title[used++] = c;
        }
    }
}

static void _get_status(char *out, size_t cap) {
    pthread_mutex_lock(&_lock);
    snprintf(out, cap, "%s", _status);
    pthread_mutex_unlock(&_lock);
}

// code.py or boot.py running: a file name, not "12@code.py ValueError" (it ended).
static bool _running(void) {
    char s[64];
    _get_status(s, sizeof(s));
    size_t n = strlen(s);
    return n > 3 && strcmp(s + n - 3, ".py") == 0 && strchr(s, '@') == NULL;
}

// Where a reset leaves the port for the process it starts (supervisor/port.c, reset_cpu()).
#define RESTART_PORT_ENV "CIRCUITPY_API_RESTART_PORT"

static void _set_env(const char *name, const char *value) {
    #ifdef _WIN32
    SetEnvironmentVariableA(name, value);
    _putenv_s(name, value != NULL ? value : "");
    #else
    if (value != NULL) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
    #endif
}

static void _hex(char *out, const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", buf[i]);
    }
}

// ---- answers ----

static void _send_all(sock_t s, const char *data, size_t len) {
    while (len > 0) {
        int n = send(s, data, len > 65536 ? 65536 : (int)len, 0);
        if (n <= 0) {
            return;
        }
        data += n;
        len -= (size_t)n;
    }
}

static const char *_reason(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 409:
            return "Conflict";
        case 413:
            return "Payload Too Large";
        case 503:
            return "Service Unavailable";
        default:
            return "Error";
    }
}

static void _answer(request_t *r, int status, const char *type, const char *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
        status, _reason(status), type, (unsigned long)len);
    _send_all(r->sock, head, (size_t)n);
    _send_all(r->sock, body, len);
}

static void _answer_json(request_t *r, int status, api_buf_t *b) {
    if (b->failed) {
        const char *oom = "{\"error\": \"out of memory\"}";
        _answer(r, 503, "application/json", oom, strlen(oom));
    } else {
        _answer(r, status, "application/json", b->data ? b->data : "{}", b->data ? b->len : 2);
    }
}

static void _error(request_t *r, int status, const char *message) {
    api_buf_t b = {0};
    api_buf_add(&b, "{\"error\": ", 10);
    api_buf_json_string(&b, message, strlen(message));
    api_buf_add(&b, "}", 1);
    _answer_json(r, status, &b);
    api_buf_free(&b);
}

// ---- endpoints ----

// v in decimal: the VM's printf, which the port's snprintf is, has no %llu.
static const char *_u64(char out[24], uint64_t v) {
    char *p = out + 23;
    *p = '\0';
    do {
        *--p = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    return p;
}

static void _info(request_t *r) {
    api_buf_t b = {0};
    api_buf_add(&b, "{\"version\": ", 12);
    api_buf_json_string(&b, MICROPY_GIT_TAG, strlen(MICROPY_GIT_TAG));
    api_buf_add(&b, ", \"board\": ", 11);
    api_buf_json_string(&b, desktop_options.board_name, strlen(desktop_options.board_name));
    api_buf_add(&b, ", \"board_file\": ", 16);
    const char *board_file = desktop_options.board_file_count > 0 ? desktop_options.board_files[0] : NULL;
    api_buf_json_string(&b, board_file, board_file ? strlen(board_file) : 0);
    api_buf_add(&b, ", \"drive\": ", 11);
    api_buf_json_string(&b, desktop_options.drive_path, strlen(desktop_options.drive_path));
    char status[64];
    _get_status(status, sizeof(status));
    char skipped[24];
    api_buf_printf(&b, ", \"fast_time\": %s, \"skipped_ms\": %s", desktop_options.fast_time ? "true" : "false",
        _u64(skipped, desktop_skipped_ns() / 1000000));
    api_buf_printf(&b, ", \"pid\": %d, \"boot\": \"%s\", \"status\": ", (int)getpid(), _boot);
    api_buf_json_string(&b, status, strlen(status));
    api_buf_add(&b, "}", 1);
    _answer_json(r, 200, &b);
    api_buf_free(&b);
}

// The query parameter name, %-decoded into out; false when absent.
static bool _param(const request_t *r, const char *name, char *out, size_t cap) {
    size_t n = strlen(name);
    for (const char *p = r->query; *p != '\0';) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if (strncmp(p, name, n) == 0 && (p[n] == '=' || p + n == end)) {
            const char *v = p[n] == '=' ? p + n + 1 : end;
            size_t o = 0;
            while (v < end && o + 1 < cap) {
                if (*v == '%' && end - v >= 3) {
                    char hex[3] = { v[1], v[2], '\0' };
                    out[o++] = (char)strtol(hex, NULL, 16);
                    v += 3;
                } else {
                    out[o++] = *v == '+' ? ' ' : *v;
                    v++;
                }
            }
            out[o] = '\0';
            return true;
        }
        p = amp ? amp + 1 : end;
    }
    return false;
}

static long long _param_int(const request_t *r, const char *name, long long fallback) {
    char v[32];
    return _param(r, name, v, sizeof(v)) && v[0] != '\0' ? strtoll(v, NULL, 10) : fallback;
}

// GET a stream from since: what came, and with wait= when the text came, as JSON. since is
// counted in the run boot= names; from another run (before a reset) it reads from the start.
static void _stream(request_t *r, api_ring_t *ring) {
    char text[512];
    bool wait = _param(r, "wait", text, sizeof(text)) && text[0] != '\0';
    long long since = _param_int(r, "since", 0);
    char boot[sizeof(_boot)];
    if (_param(r, "boot", boot, sizeof(boot)) && boot[0] != '\0' && strcmp(boot, _boot) != 0) {
        since = 0;
    }
    long long timeout = _param_int(r, "timeout", 0);
    long long max = _param_int(r, "max", RING_SIZE);
    timeout = timeout < 0 ? 0 : timeout > WAIT_MAX_MS ? WAIT_MAX_MS : timeout;
    char *data = NULL;
    api_ring_read_t info;
    size_t n = api_ring_read(ring, since < 0 ? 0 : (uint64_t)since, wait ? text : NULL, (int)timeout,
        max <= 0 ? RING_SIZE : (size_t)max, &data, &info);
    api_buf_t b = {0};
    char start[24], end[24], next[24];
    api_buf_printf(&b, "{\"boot\": \"%s\", \"start\": %s, \"end\": %s, \"next\": %s, \"lost\": %s, \"matched\": %s, \"data\": ", _boot,
        _u64(start, info.start), _u64(end, info.end), _u64(next, info.next),
        info.lost ? "true" : "false", info.matched ? "true" : "false");
    api_buf_json_string(&b, data ? data : "", n);
    api_buf_add(&b, "}", 1);
    free(data);
    _answer_json(r, 200, &b);
    api_buf_free(&b);
}

static void _ok(request_t *r) {
    _answer(r, 200, "application/json", "{}", 2);
}

// Waits up to ms for text on the console after offset from; true when it came.
static bool _console_wait(uint64_t from, const char *text, int ms) {
    char *data = NULL;
    api_ring_read_t info;
    api_ring_read(&_console, from, text, ms, 0, &data, &info);
    free(data);
    return info.matched;
}

static void _type(const char *s) {
    desktop_serial_inject((const uint8_t *)s, strlen(s));
}

// Ctrl-C to a running code.py, until it has stopped at "press any key".
static void _stop_program(void) {
    uint64_t from = api_ring_end(&_console);
    _type("\x03");
    _console_wait(from, "Press any key", 3000);
}

// ---- exec ----

// Four hex digits at p (the string's end stops it, a NUL being no digit).
static bool _hex4(const char *p, unsigned *out) {
    *out = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
            return false;
        }
        *out = *out * 16 + (unsigned)d;
    }
    return true;
}

// The value of key in a flat JSON object: a string (unescaped, malloc'd into *str), a number or
// a boolean (into *num). False when the key is absent or not of that kind.
static bool _json_field(const char *json, const char *key, char **str, double *num) {
    char quoted[64];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char *p = strstr(json, quoted);
    if (p == NULL) {
        return false;
    }
    p += strlen(quoted);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') {
        p++;
    }
    if (num != NULL) {
        if (strncmp(p, "true", 4) == 0 || strncmp(p, "false", 5) == 0) {
            *num = p[0] == 't';
            return true;
        }
        char *end;
        *num = strtod(p, &end);
        return end != p;
    }
    if (*p != '"') {
        return false;
    }
    api_buf_t b = {0};
    for (p++; *p != '\0' && *p != '"'; p++) {
        if (*p != '\\') {
            api_buf_add(&b, p, 1);
            continue;
        }
        if (p[1] == '\0') {
            break;              // a backslash last: the string is cut
        }
        p++;
        char c = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? '\t' : *p == 'b' ? '\b' : *p == 'f' ? '\f' : *p;
        if (*p != 'u') {
            api_buf_add(&b, &c, 1);
            continue;
        }
        // \uXXXX, a surrogate pair as one character, written as UTF-8; anything malformed U+FFFD
        unsigned cp, lo;
        if (!_hex4(p + 1, &cp)) {
            cp = 0xfffd;
        } else {
            p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && p[1] == '\\' && p[2] == 'u' && _hex4(p + 3, &lo)
                && lo >= 0xdc00 && lo < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                p += 6;
            } else if (cp >= 0xd800 && cp < 0xe000) {
                cp = 0xfffd;
            }
        }
        char u[4];
        int n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (n == 1) {
            u[0] = (char)cp;
        } else {
            for (int i = n - 1; i > 0; i--) {
                u[i] = (char)(0x80 | (cp & 0x3f));
                cp >>= 6;
            }
            u[0] = (char)((n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) | cp);
        }
        api_buf_add(&b, u, (size_t)n);
    }
    api_buf_add(&b, "", 0);
    *str = b.data ? b.data : calloc(1, 1);
    return !b.failed;
}

// s as a Python string literal in ASCII: quotes, backslashes and control characters escaped
// (Ctrl-C and Ctrl-D would act on the way in, a newline would end the REPL's line), and what is
// not ASCII as \u or \U escapes, which the REPL's line editor passes unchanged.
static void _literal(api_buf_t *b, const char *s, size_t n) {
    api_buf_add(b, "'", 1);
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = u[i];
        if (c == '\\' || c == '\'') {
            char esc[2] = { '\\', (char)c };
            api_buf_add(b, esc, 2);
        } else if (c < 0x20 || c == 0x7f) {
            api_buf_printf(b, "\\x%02x", c);
        } else if (c < 0x80) {
            api_buf_add(b, s + i, 1);
        } else {
            int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
            unsigned cp = more == 3 ? c & 0x07 : more == 2 ? c & 0x0f : more == 1 ? c & 0x1f : 0xfffd;
            for (int k = 0; k < more && i + 1 < n && (u[i + 1] & 0xc0) == 0x80; k++) {
                cp = (cp << 6) | (u[++i] & 0x3f);
            }
            if (cp > 0xffff) {
                api_buf_printf(b, "\\U%08x", cp);
            } else {
                api_buf_printf(b, "\\u%04x", cp);
            }
        }
    }
    api_buf_add(b, "'", 1);
}

// What the code's run prints around its output: start, value, error, done (ASCII RS and a letter;
// the echoed line holds them escaped, so only the run prints them).
#define MARK_START "\x1eS"
#define MARK_VALUE "\x1eV"
#define MARK_ERROR "\x1e" "E"
#define MARK_DONE "\x1e" "D"

// The code runs in the friendly REPL as one typed line, so it shares the REPL's variables (the
// raw REPL would start a new VM for each run). The value of an expression is printed after a
// mark, an exception after another.
static const char _runner[] =
    "def __api(c):\n"
    "    print('\\x1eS')\n"
    "    try:\n"
    "        try:\n"
    "            v = compile(c, '<api>', 'eval')\n"
    "        except SyntaxError:\n"
    "            exec(compile(c, '<api>', 'exec'), globals())\n"
    "        else:\n"
    "            v = eval(v, globals())\n"
    "            if v is not None:\n"
    "                print('\\x1eV' + repr(v))\n"
    "    except BaseException as e:\n"
    "        print('\\x1eE', end='')\n"
    "        import traceback\n"
    "        traceback.print_exception(e)\n"
    "    print('\\x1eD')\n";

static void _exec_line(api_buf_t *line, const char *code) {
    api_buf_t source = {0};
    api_buf_add(&source, _runner, strlen(_runner));
    api_buf_add(&source, "__api(", 6);
    _literal(&source, code, strlen(code));
    api_buf_add(&source, ")\ndel __api\n", 12);
    api_buf_add(line, "exec(", 5);
    _literal(line, source.data, source.len);
    api_buf_add(line, ", globals())\r", 13);
    line->failed |= source.failed;
    api_buf_free(&source);
}

// The first mark in [p, end), or end.
static const char *_mark(const char *p, const char *end, const char *mark) {
    for (; p + 2 <= end; p++) {
        if (p[0] == mark[0] && p[1] == mark[1]) {
            return p;
        }
    }
    return end;
}

static pthread_mutex_t _exec_lock = PTHREAD_MUTEX_INITIALIZER;

static void _exec(request_t *r) {
    char *code = NULL;
    double timeout_ms = 10000, interrupt = 0;
    if (r->body == NULL || !_json_field(r->body, "code", &code, NULL)) {
        _error(r, 400, "the body needs \"code\", a string");
        return;
    }
    _json_field(r->body, "timeout_ms", NULL, &timeout_ms);
    _json_field(r->body, "interrupt", NULL, &interrupt);
    // Within 1 ms and 10 minutes (and not NaN, which fails both tests).
    int wait_ms = timeout_ms >= 1 && timeout_ms <= EXEC_MAX_MS ? (int)timeout_ms : timeout_ms > 1 ? EXEC_MAX_MS : 1;
    api_buf_t answer = {0};
    // One at a time; a client gone away still lets its exec finish and unlock.
    pthread_mutex_lock(&_exec_lock);
    const char *fail = NULL;
    int status = 200;
    uint64_t from;
    if (_running()) {
        if ((int)interrupt == 0) {
            status = 409;
            fail = "program running";
            goto done;
        }
        _stop_program();
    }
    char now[64];
    _get_status(now, sizeof(now));
    if (strcmp(now, "REPL") != 0) {
        // "Done" or ended with an error: a key enters the REPL.
        from = api_ring_end(&_console);
        _type("\r");
        if (!_console_wait(from, ">>> ", 5000)) {
            status = 503;
            fail = "the REPL did not start";
            goto done;
        }
    }
    api_buf_t line = {0};
    _exec_line(&line, code);
    if (line.failed) {
        api_buf_free(&line);
        status = 503;
        fail = "out of memory";
        goto done;
    }
    from = api_ring_end(&_console);
    desktop_serial_inject((const uint8_t *)line.data, line.len);
    api_buf_free(&line);
    bool finished = _console_wait(from, MARK_DONE, wait_ms);
    if (!finished) {
        _type("\x03");
        _console_wait(from, MARK_DONE, 3000);
    }
    _console_wait(from, ">>> ", 3000);

    char *data = NULL;
    api_ring_read_t info;
    size_t n = api_ring_read(&_console, from, NULL, 0, RING_SIZE, &data, &info);
    const char *end = data ? data + n : NULL;
    const char *start = data ? _mark(data, end, MARK_START) : NULL;
    // The output begins on the line after the start mark.
    const char *out = start;
    while (out != NULL && out < end && *out != '\n') {
        out++;
    }
    out = out != NULL && out < end ? out + 1 : end;
    const char *done_mark = out ? _mark(out, end, MARK_DONE) : NULL;
    const char *value = out ? _mark(out, done_mark, MARK_VALUE) : NULL;
    const char *err = out ? _mark(out, done_mark, MARK_ERROR) : NULL;
    const char *out_end = value < err ? value : err;
    const char *value_end = value;
    while (value_end != NULL && value_end < done_mark && *value_end != '\r' && *value_end != '\n') {
        value_end++;
    }

    api_buf_t b = {0};
    bool raised = err != NULL && err < done_mark;
    api_buf_printf(&b, "{\"ok\": %s, \"output\": ", finished && !raised ? "true" : "false");
    api_buf_json_string(&b, out ? out : "", out ? (size_t)(out_end - out) : 0);
    api_buf_add(&b, ", \"value\": ", 11);
    api_buf_json_string(&b, value != NULL && value < done_mark ? value + 2 : NULL,
        value != NULL && value < done_mark ? (size_t)(value_end - value - 2) : 0);
    api_buf_add(&b, ", \"error\": ", 11);
    if (!finished) {
        api_buf_add(&b, "\"timeout\"", 9);
    } else {
        api_buf_json_string(&b, raised ? err + 2 : NULL, raised ? (size_t)(done_mark - err - 2) : 0);
    }
    api_buf_add(&b, "}", 1);
    free(data);
    answer = b;
done:
    // The answer goes out after the lock is free: a slow reader does not hold up the next exec.
    pthread_mutex_unlock(&_exec_lock);
    free(code);
    if (fail != NULL) {
        _error(r, status, fail);
    } else {
        _answer_json(r, 200, &answer);
    }
    api_buf_free(&answer);
}

// ---- the VM's part: the panel, the state, the displays ----

typedef enum {
    VM_STATE,
    VM_PANEL,
    VM_DISPLAY,                 // in: the display's name; out: width, height (16 bits each), RGB565
} vm_kind_t;

// A request the VM runs: the server thread queues it and waits; whichever of the two is done
// with it last frees it, so a request given up on is still safe for the VM to finish.
typedef struct {
    vm_kind_t kind;
    const char *in;
    api_buf_t out;
    int status;
    bool done;
    int refs;
} vm_request_t;

#define VM_QUEUE 16
#define VM_REPLY_MS 2000

static vm_request_t *_queue[VM_QUEUE];
static int _queued;
static pthread_cond_t _vm_done = PTHREAD_COND_INITIALIZER;

static void _release(vm_request_t *q) {
    if (--q->refs == 0) {
        free((void *)q->in);
        api_buf_free(&q->out);
        free(q);
    }
}

#if CIRCUITPY_SDL
static void _vm_display(vm_request_t *q) {
    uint16_t w = 0, h = 0;
    const uint16_t *pixels = NULL;
    bool on = true;
    if (strcmp(q->in, "DISPLAY") == 0) {
        desktop_sdl_frame(&w, &h, &pixels);
    } else {
        const vdisplay_t *d = vdisplay_by_name(q->in);
        if (d != NULL) {
            w = d->width;
            h = d->height;
            pixels = d->pixels;
            on = d->on;
        }
    }
    if (pixels == NULL) {
        q->status = 404;
        return;
    }
    uint16_t size[2] = { w, h };
    api_buf_add(&q->out, (const char *)size, sizeof(size));
    size_t bytes = (size_t)w * h * 2;
    if (on) {
        api_buf_add(&q->out, (const char *)pixels, bytes);
    } else {
        // As the panel shows it: an off display is dark.
        static const char dark[512];
        for (size_t done = 0; done < bytes; done += sizeof(dark)) {
            api_buf_add(&q->out, dark, bytes - done < sizeof(dark) ? bytes - done : sizeof(dark));
        }
    }
}

static void _vm_panel(vm_request_t *q) {
    api_buf_add(&q->out, "{\"results\": [", 13);
    bool first = true, all = true;
    const char *p = q->in;
    while (*p != '\0') {
        const char *eol = p + strcspn(p, "\r\n");
        char line[256];
        size_t n = (size_t)(eol - p) < sizeof(line) - 1 ? (size_t)(eol - p) : sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p = *eol != '\0' ? eol + 1 : eol;
        if (strspn(line, " \t") == n) {
            continue;
        }
        char err[96] = "";
        bool ok = panel_apply(line, err, sizeof(err));
        all = all && ok;
        api_buf_add(&q->out, first ? "{\"line\": " : ", {\"line\": ", first ? 9 : 11);
        api_buf_json_string(&q->out, line, n);
        api_buf_printf(&q->out, ", \"ok\": %s, \"error\": ", ok ? "true" : "false");
        api_buf_json_string(&q->out, ok ? NULL : err, strlen(err));
        api_buf_add(&q->out, "}", 1);
        first = false;
    }
    api_buf_add(&q->out, "]}", 2);
    q->status = all ? 200 : 400;
}
#endif

void control_api_vm_task(void) {
    if (__atomic_load_n(&_queued, __ATOMIC_ACQUIRE) == 0) {
        return;
    }
    vm_request_t *work[VM_QUEUE];
    pthread_mutex_lock(&_lock);
    int n = _queued;
    memcpy(work, _queue, sizeof(work[0]) * (size_t)n);
    __atomic_store_n(&_queued, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&_lock);
    for (int i = 0; i < n; i++) {
        vm_request_t *q = work[i];
        q->status = 200;
        #if CIRCUITPY_SDL
        if (q->kind == VM_STATE) {
            panel_state_json(&q->out);
        } else if (q->kind == VM_PANEL) {
            _vm_panel(q);
        } else if (q->kind == VM_DISPLAY) {
            _vm_display(q);
        }
        #endif
        pthread_mutex_lock(&_lock);
        q->done = true;
        pthread_cond_broadcast(&_vm_done);
        _release(q);
        pthread_mutex_unlock(&_lock);
    }
}

// Runs a request on the VM thread and answers with what it wrote; 503 when the VM does not get to
// it in time.
static void _vm_call(request_t *r, vm_kind_t kind, const char *in) {
    vm_request_t *q = calloc(1, sizeof(*q));
    char *copy = in ? strdup(in) : NULL;
    if (q == NULL || (in != NULL && copy == NULL)) {
        free(q);
        free(copy);
        _error(r, 503, "out of memory");
        return;
    }
    q->kind = kind;
    q->in = copy;
    q->refs = 2;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += VM_REPLY_MS / 1000;
    pthread_mutex_lock(&_lock);
    if (_queued == VM_QUEUE) {
        pthread_mutex_unlock(&_lock);
        q->refs = 1;
        _release(q);
        _error(r, 503, "too many requests waiting for the VM");
        return;
    }
    _queue[_queued] = q;
    __atomic_store_n(&_queued, _queued + 1, __ATOMIC_RELEASE);
    desktop_wake_vm();
    while (!q->done && pthread_cond_timedwait(&_vm_done, &_lock, &deadline) == 0) {
    }
    bool done = q->done;
    pthread_mutex_unlock(&_lock);
    if (!done) {
        _error(r, 503, "VM not responding");
    } else if (q->out.failed) {
        _error(r, 503, "out of memory");
    } else if (kind == VM_DISPLAY && q->status == 404) {
        _error(r, 404, "no such display");
    } else if (kind == VM_DISPLAY) {
        uint16_t size[2];
        memcpy(size, q->out.data, sizeof(size));
        uint8_t *png = NULL;
        size_t n = png_write_rgb565((const uint16_t *)(q->out.data + sizeof(size)), size[0], size[1], &png);
        if (n == 0) {
            _error(r, 503, "out of memory");
        } else {
            _answer(r, 200, "image/png", (const char *)png, n);
        }
        free(png);
    } else {
        _answer(r, q->status, "application/json", q->out.data ? q->out.data : "{}", q->out.data ? q->out.len : 2);
    }
    pthread_mutex_lock(&_lock);
    _release(q);
    pthread_mutex_unlock(&_lock);
}

// The panel's endpoints exist on the virtual board, where its parts are.
static bool _panel_board(request_t *r) {
    #if CIRCUITPY_SDL
    if (strcmp(desktop_options.board_name, "virtual") == 0) {
        return true;
    }
    #endif
    _error(r, 404, "no panel on this board");
    return false;
}

static void _reload(request_t *r) {
    // A running program takes Ctrl-C first; Ctrl-D then reloads from "press any key".
    if (_running()) {
        _stop_program();
    }
    _type("\x04");
    _ok(r);
}

static void _quit(request_t *r) {
    const char *ok = "{}";
    _answer(r, 200, "application/json", ok, 2);
    close_socket(r->sock);
    desktop_power_off(0);
}

static void _route(request_t *r) {
    bool get = strcmp(r->method, "GET") == 0, post = strcmp(r->method, "POST") == 0;
    if (strcmp(r->path, "/api/info") == 0 && get) {
        _info(r);
    } else if (strcmp(r->path, "/api/quit") == 0 && post) {
        _quit(r);
    } else if (strcmp(r->path, "/api/console") == 0 && get) {
        _stream(r, &_console);
    } else if (strcmp(r->path, "/api/trace") == 0 && get) {
        _stream(r, &_trace);
    } else if (strcmp(r->path, "/api/console") == 0 && post) {
        size_t written = desktop_serial_inject((const uint8_t *)r->body, r->body_len);
        api_buf_t b = {0};
        api_buf_printf(&b, "{\"written\": %lu}", (unsigned long)written);
        _answer_json(r, 200, &b);
        api_buf_free(&b);
    } else if (strcmp(r->path, "/api/interrupt") == 0 && post) {
        _type("\x03");
        _ok(r);
    } else if (strcmp(r->path, "/api/reload") == 0 && post) {
        _reload(r);
    } else if (strcmp(r->path, "/api/exec") == 0 && post) {
        _exec(r);
    } else if (strcmp(r->path, "/api/state") == 0 && get) {
        if (_panel_board(r)) {
            _vm_call(r, VM_STATE, NULL);
        }
    } else if (strcmp(r->path, "/api/display") == 0 && get) {
        char name[32];
        if (!_param(r, "name", name, sizeof(name)) || name[0] == '\0') {
            strcpy(name, "DISPLAY");
        }
        if (_panel_board(r)) {
            _vm_call(r, VM_DISPLAY, name);
        }
    } else if (strcmp(r->path, "/api/panel") == 0 && post) {
        if (_panel_board(r)) {
            _vm_call(r, VM_PANEL, r->body ? r->body : "");
        }
    } else {
        _error(r, 404, "no such endpoint");
    }
}

// ---- requests ----

// The value of header name in head (lines after the first), trimmed; false when absent.
static bool _header(const char *head, const char *name, char *out, size_t cap) {
    size_t n = strlen(name);
    for (const char *line = strstr(head, "\r\n"); line != NULL; line = strstr(line, "\r\n")) {
        line += 2;
        if (strncasecmp(line, name, n) == 0 && line[n] == ':') {
            const char *v = line + n + 1;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            const char *end = strstr(v, "\r\n");
            size_t len = end ? (size_t)(end - v) : strlen(v);
            while (len > 0 && (v[len - 1] == ' ' || v[len - 1] == '\t')) {
                len--;
            }
            if (len >= cap) {
                return false;
            }
            memcpy(out, v, len);
            out[len] = '\0';
            return true;
        }
    }
    return false;
}

// The Host header names this server: the bound address or localhost, with the port or without.
static bool _host_ok(const char *host) {
    char name[96];
    if (strlen(host) >= sizeof(name)) {
        return false;
    }
    strcpy(name, host);
    char *colon = strrchr(name, ':');
    if (colon != NULL && strchr(name, ']') == NULL) {
        if (atoi(colon + 1) != _port) {
            return false;
        }
        *colon = '\0';
    }
    return strcmp(name, _host) == 0 || strcmp(name, "localhost") == 0 || strcmp(name, "127.0.0.1") == 0;
}

// The token compared without stopping at the first difference.
static bool _token_ok(const char *auth) {
    const char *prefix = "Bearer ";
    if (strncmp(auth, prefix, 7) != 0 || strlen(auth + 7) != strlen(_token)) {
        return false;
    }
    unsigned diff = 0;
    for (size_t i = 0; _token[i] != '\0'; i++) {
        diff |= (unsigned)(auth[7 + i] ^ _token[i]);
    }
    return diff == 0;
}

static void _serve(sock_t s) {
    request_t r = { .sock = s };
    char *head = malloc(HEAD_MAX + 1);
    size_t got = 0;
    char *end = NULL;
    while (head != NULL && got < HEAD_MAX) {
        int n = recv(s, head + got, HEAD_MAX - got, 0);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
        head[got] = '\0';
        if ((end = strstr(head, "\r\n\r\n")) != NULL) {
            break;
        }
    }
    char target[800];
    if (end == NULL || sscanf(head, "%7s %799s", r.method, target) != 2) {
        free(head);
        return;
    }
    end[2] = '\0';              // the headers end with their last \r\n
    char *q = strchr(target, '?');
    if (q != NULL) {
        *q++ = '\0';
    }
    if (strlen(target) >= sizeof(r.path) || (q != NULL && strlen(q) >= sizeof(r.query))) {
        _error(&r, 400, "target too long");
        free(head);
        return;
    }
    strcpy(r.path, target);
    strcpy(r.query, q != NULL ? q : "");

    char value[512];
    size_t length = 0;
    if (_header(head, "Content-Length", value, sizeof(value))) {
        length = strtoul(value, NULL, 10);
    }
    if (strcmp(r.method, "GET") != 0 && strcmp(r.method, "POST") != 0) {
        _error(&r, 405, "GET and POST only");
    } else if (desktop_options.api_host == NULL && (!_header(head, "Host", value, sizeof(value)) || !_host_ok(value))) {
        // On 127.0.0.1 a page whose name resolves there (DNS rebinding) is refused. Elsewhere
        // (--api-host) clients name the computer as they reach it; the user's token guards it.
        _error(&r, 403, "Host is not this server");
    } else if (!_header(head, "Authorization", value, sizeof(value)) || !_token_ok(value)) {
        _error(&r, 401, "a Bearer token is needed");
    } else if (length > BODY_MAX) {
        _error(&r, 413, "body too large");
    } else {
        // The body: what came with the headers, then the rest.
        size_t have = got - (size_t)(end + 4 - head);
        r.body = malloc(length + 1);
        if (r.body != NULL) {
            memcpy(r.body, end + 4, have < length ? have : length);
            r.body_len = have < length ? have : length;
            while (r.body_len < length) {
                int n = recv(s, r.body + r.body_len, length - r.body_len, 0);
                if (n <= 0) {
                    break;
                }
                r.body_len += (size_t)n;
            }
            r.body[r.body_len] = '\0';
            _route(&r);
        }
        free(r.body);
    }
    free(head);
}

static void *_connection(void *arg) {
    sock_t s = (sock_t)(intptr_t)arg;
    _serve(s);
    close_socket(s);
    pthread_mutex_lock(&_lock);
    _connections--;
    pthread_cond_signal(&_slot_free);
    pthread_mutex_unlock(&_lock);
    return NULL;
}

static void *_accept_loop(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&_lock);
        while (_connections >= CONNECTIONS_MAX) {
            pthread_cond_wait(&_slot_free, &_lock);
        }
        pthread_mutex_unlock(&_lock);
        #ifdef _WIN32
        sock_t s = accept(_listener, NULL, NULL);
        #elif defined(__APPLE__)
        sock_t s = accept(_listener, NULL, NULL);
        if (s != BAD_SOCKET) {
            fcntl(s, F_SETFD, FD_CLOEXEC);      // not kept by a reset's exec
        }
        #else
        sock_t s = accept4(_listener, NULL, NULL, SOCK_CLOEXEC);     // not kept by a reset's exec
        #endif
        if (s == BAD_SOCKET) {
            if (_listener == BAD_SOCKET) {
                return NULL;        // closed for a reset
            }
            // A connection reset before it was taken, or out of descriptors: try again.
            struct timespec t = { 0, 10000000L };
            nanosleep(&t, NULL);
            continue;
        }
        #ifdef _WIN32
        // Not inherited by the process a reset starts (it inherits handles).
        SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
        DWORD io_ms = IO_TIMEOUT_MS;
        #else
        struct timeval io_ms = { IO_TIMEOUT_MS / 1000, 0 };
        #endif
        // A client that sends or reads nothing gives its slot back.
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&io_ms, sizeof(io_ms));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&io_ms, sizeof(io_ms));
        pthread_mutex_lock(&_lock);
        _connections++;
        pthread_mutex_unlock(&_lock);
        pthread_t t;
        if (pthread_create(&t, NULL, _connection, (void *)(intptr_t)s) == 0) {
            pthread_detach(t);
        } else {
            close_socket(s);
            pthread_mutex_lock(&_lock);
            _connections--;
            pthread_mutex_unlock(&_lock);
        }
    }
}

bool control_api_start(void) {
    if (desktop_options.api_port < 0) {
        return true;
    }
    const char *token = getenv("CIRCUITPY_API_TOKEN");
    const char *host = desktop_options.api_host ? desktop_options.api_host : "127.0.0.1";
    if (desktop_options.api_host != NULL && (token == NULL || token[0] == '\0')) {
        fprintf(stderr, "circuitpython: --api-host needs a token of your own in CIRCUITPY_API_TOKEN\n");
        return false;
    }
    uint8_t bytes[16];
    if (token != NULL && token[0] != '\0') {
        if (strlen(token) >= sizeof(_token)) {
            fprintf(stderr, "circuitpython: CIRCUITPY_API_TOKEN: at most %d characters\n", (int)sizeof(_token) - 1);
            return false;
        }
        strcpy(_token, token);
    } else if (common_hal_os_urandom(bytes, sizeof(bytes))) {
        _hex(_token, bytes, sizeof(bytes));
    } else {
        fprintf(stderr, "circuitpython: api: no random numbers for a token\n");
        return false;
    }
    if (!common_hal_os_urandom(bytes, 4)) {
        memset(bytes, 0, 4);
    }
    _hex(_boot, bytes, 4);
    snprintf(_host, sizeof(_host), "%s", host);
    if (!api_ring_init(&_console, RING_SIZE) || !api_ring_init(&_trace, RING_SIZE)) {
        fprintf(stderr, "circuitpython: api: out of memory\n");
        return false;
    }
    _on = true;
    bus_trace_to_api();

    // A process started by a reset takes the port the one before it had, also for --api 0.
    const char *again = getenv(RESTART_PORT_ENV);
    int port = again != NULL && again[0] != '\0' ? atoi(again) : desktop_options.api_port;
    _set_env(RESTART_PORT_ENV, NULL);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "circuitpython: --api-host %s: not an IPv4 address\n", host);
        return false;
    }
    #ifdef _WIN32
    // Not inherited: a process started by a reset binds the port itself.
    _listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    #else
    #ifdef __APPLE__
    _listener = socket(AF_INET, SOCK_STREAM, 0);     // macOS has no SOCK_CLOEXEC
    fcntl(_listener, F_SETFD, FD_CLOEXEC);
    #else
    _listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    #endif
    int one = 1;
    setsockopt(_listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    #endif
    socklen_t len = sizeof(addr);
    if (_listener == BAD_SOCKET || bind(_listener, (struct sockaddr *)&addr, sizeof(addr)) != 0
        || listen(_listener, 16) != 0 || getsockname(_listener, (struct sockaddr *)&addr, &len) != 0) {
        fprintf(stderr, "circuitpython: api: cannot listen on %s:%d\n", host, port);
        return false;
    }
    _port = ntohs(addr.sin_port);
    pthread_t t;
    if (pthread_create(&t, NULL, _accept_loop, NULL) != 0) {
        fprintf(stderr, "circuitpython: api: no thread\n");
        return false;
    }
    pthread_detach(t);
    fprintf(stderr, "circuitpython: api http://%s:%d/ token %s\n", host, _port, _token);
    fflush(stderr);
    return true;
}

void control_api_stop(void) {
    if (_listener != BAD_SOCKET) {
        close_socket(_listener);
        _listener = BAD_SOCKET;
        char port[16];
        snprintf(port, sizeof(port), "%d", _port);
        _set_env(RESTART_PORT_ENV, port);
        _set_env("CIRCUITPY_API_TOKEN", _token);
    }
}
