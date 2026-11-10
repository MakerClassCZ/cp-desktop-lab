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

#include "after_run.h"
#include "fault.h"
#include "heap_walk.h"
#include "audio_capture.h"
#include "api_json.h"
#include "api_ring.h"
#include "ble_controller.h"
#include "common-hal/_bleio/Adapter.h"
#include "common-hal/_bleio/bonding.h"
#include "common-hal/_bleio/smp_crypto.h"
#include "shared-bindings/_bleio/__init__.h"
#include "ble_host.h"
#include "bus_trace.h"
#include "desktop_port.h"
#include "platform.h"
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
static uint64_t _console_served;   // the console's end as last sent to a reader; 0: no reader
// The supervisor's status, from the terminal title it writes: "code.py", "REPL", "Done" ...
static char _status[64] = "starting";

// ---- the console's output ----

// The title escape, ESC ] 0 ; ... ESC \, is "🐍<ip> | <BLE:name> | <status> | <version>", or the
// same with single spaces (adafruit/circuitpython#11717). The ip and BLE fields are optional; the
// status is the field before the version and may hold spaces ("12@code.py ValueError").
static void _title(const char *title) {
    static const char snake[] = "\xf0\x9f\x90\x8d";
    const char *a = strncmp(title, snake, 4) == 0 ? title + 4 : title;
    const char *b = strstr(a, " | ");
    if (b != NULL) {
        for (const char *c; (c = strstr(b + 3, " | ")) != NULL; a = b + 3, b = c) {
        }
    } else {
        b = strrchr(a, ' ');
        a += strspn(a, " ");
        size_t ip = strspn(a, "0123456789.:");
        if (ip > 0 && a[ip] == ' ') {
            a += ip + 1;
        }
        const char *ble = strncmp(a, "BLE:", 4) == 0 ? strchr(a, ' ') : NULL;
        if (ble != NULL && ble < b) {
            a = ble + 1;
        }
    }
    if (b != NULL && b > a) {
        pthread_mutex_lock(&_lock);
        size_t n = (size_t)(b - a) < sizeof(_status) - 1 ? (size_t)(b - a) : sizeof(_status) - 1;
        memcpy(_status, a, n);
        _status[n] = '\0';
        pthread_mutex_unlock(&_lock);
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

void control_api_info_json(api_buf_t *b) {
    api_buf_add(b, "{\"version\": ", 12);
    api_buf_json_string(b, MICROPY_GIT_TAG, strlen(MICROPY_GIT_TAG));
    api_buf_printf(b, ", \"machine\": \"%s\", \"board\": ", MICROPY_HW_MCU_NAME);
    api_buf_json_string(b, desktop_options.board_name, strlen(desktop_options.board_name));
    api_buf_add(b, ", \"board_file\": ", 16);
    const char *board_file = desktop_options.board_file_count > 0 ? desktop_options.board_files[0] : NULL;
    api_buf_json_string(b, board_file, board_file ? strlen(board_file) : 0);
    api_buf_add(b, ", \"profile\": ", 13);
    const char *profile = desktop_options.profile;
    api_buf_json_string(b, profile, profile ? strlen(profile) : 0);
    api_buf_printf(b, ", \"heap_limit\": %lu, \"sockets\": %lu", (unsigned long)desktop_options.heap_limit,
        (unsigned long)desktop_options.sockets);
    api_buf_add(b, ", \"drive\": ", 11);
    api_buf_json_string(b, desktop_options.drive_path, strlen(desktop_options.drive_path));
    char status[64];
    _get_status(status, sizeof(status));
    char skipped[24];
    api_buf_printf(b, ", \"fast_time\": %s, \"skipped_ms\": %s", desktop_options.fast_time ? "true" : "false",
        _u64(skipped, desktop_skipped_ns() / 1000000));
    api_buf_printf(b, ", \"pid\": %d, \"boot\": \"%s\", \"status\": ", (int)getpid(), _boot);
    api_buf_json_string(b, status, strlen(status));
    api_buf_add(b, "}", 1);
}

static void _info(request_t *r) {
    api_buf_t b = {0};
    control_api_info_json(&b);
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
    if (ring == &_console) {
        pthread_mutex_lock(&_lock);
        _console_served = info.end > _console_served ? info.end : _console_served;
        pthread_mutex_unlock(&_lock);
    }
}

void control_api_linger(void) {
    for (int i = 0; i < 60; i++) {
        pthread_mutex_lock(&_lock);
        bool done = !_on || _console_served == 0 || _console_served >= api_ring_end(&_console);
        pthread_mutex_unlock(&_lock);
        if (done) {
            return;
        }
        struct timespec t = { 0, 5 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
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
// raw REPL would start a new VM for each run). The value of an expression, or of statements'
// last line when it is one (unindented), is printed after a mark, an exception after another.
static const char _runner[] =
    "def __api(c):\n"
    "    print('\\x1eS')\n"
    "    try:\n"
    "        h, n, t = c.rstrip().rpartition('\\n')\n"
    "        try:\n"
    "            h, v = None, compile(c, '<api>', 'eval')\n"
    "        except SyntaxError:\n"
    "            try:\n"
    "                if not n or t[:1] in ' \\t':\n"
    "                    raise SyntaxError\n"
    "                h, v = compile(h, '<api>', 'exec'), compile(t, '<api>', 'eval')\n"
    "            except SyntaxError:\n"
    "                h, v = compile(c, '<api>', 'exec'), None\n"
    "        if h is not None:\n"
    "            exec(h, globals())\n"
    "        if v is not None:\n"
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
    VM_BLE_BONDS,               // in: "1" for the keys too
    VM_BLE_ERASE,
    VM_AFTER_RUN,
    VM_FAULTS,
    VM_AUDIO,                   // in: "start", "stop" or NULL for the list
    VM_AUDIO_WAV,               // in: the recording's index
    VM_HEAP,
    VM_CLAIMED,
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

static void _buf_hex(api_buf_t *b, const uint8_t *data, size_t len);

// ---- the bond store, read and erased on the VM thread, which owns it ----

static void _address_json(api_buf_t *b, const char *key, const bt_addr_le_t *a) {
    api_buf_printf(b, "\"%s\": \"%02x:%02x:%02x:%02x:%02x:%02x\", \"%s_type\": \"%s\"", key,
        a->a.val[5], a->a.val[4], a->a.val[3], a->a.val[2], a->a.val[1], a->a.val[0], key,
        a->type == BT_ADDR_LE_PUBLIC ? "public" : "random");
}

static void _hex_json(api_buf_t *b, const char *key, const uint8_t *data, size_t n) {
    api_buf_printf(b, ", \"%s\": \"", key);
    _buf_hex(b, data, n);
    api_buf_add(b, "\"", 1);
}

// Every bond of the store: our identity, the role, the peer's identity, its flags; with keys, the
// keys too (to decrypt a capture of the air in Wireshark).
static void _vm_ble_bonds(vm_request_t *q) {
    const bool keys = q->in != NULL && strcmp(q->in, "1") == 0;
    bonding_store_t *store = bonding_store();
    api_buf_t *b = &q->out;
    api_buf_add(b, "{\"bonds\": [", 11);
    bool first = true;
    for (size_t i = 0; i < BONDING_MAX; i++) {
        const bonding_record_t *r = &store->records[i];
        if (!(r->flags & BONDING_IN_USE)) {
            continue;
        }
        api_buf_add(b, first ? "{" : ", {", first ? 1 : 3);
        first = false;
        _address_json(b, "own", &r->own);
        api_buf_printf(b, ", \"role\": \"%s\", ", r->role == BONDING_ROLE_PERIPHERAL ? "peripheral" : "central");
        _address_json(b, "peer", &r->peer);
        api_buf_printf(b, ", \"irk\": %s, \"authenticated\": %s, \"secure_connections\": %s, "
            "\"key_size\": %u, \"ediv\": %u, \"use\": %lu, \"cccds\": %u",
            (r->flags & BONDING_PEER_IRK) ? "true" : "false",
            (r->flags & BONDING_AUTHENTICATED) ? "true" : "false",
            (r->flags & BONDING_SC) ? "true" : "false", r->key_size, r->ediv, (unsigned long)r->use,
            r->cccd_count);
        if (keys) {
            _hex_json(b, "ltk", r->ltk, sizeof(r->ltk));
            _hex_json(b, "rand", r->rand, sizeof(r->rand));
            if (r->flags & BONDING_PEER_IRK) {
                _hex_json(b, "peer_irk", r->peer_irk, sizeof(r->peer_irk));
            }
        }
        api_buf_add(b, "}", 1);
    }
    api_buf_add(b, "]", 1);
    if (keys && (store->header.flags & BONDING_HAVE_IRK)) {
        _hex_json(b, "irk", store->header.irk, sizeof(store->header.irk));
    }
    api_buf_add(b, "}", 1);
}

// erase_bonding(): the bonds of the controller's identity, as the program's call erases them.
static void _vm_ble_erase(vm_request_t *q) {
    bleio_adapter_obj_t *adapter = &common_hal_bleio_adapter_obj;
    size_t n = bonding_erase(adapter->identity_known ? &adapter->identity : NULL);
    api_buf_printf(&q->out, "{\"erased\": %u}", (unsigned)n);
}

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
        if (q->kind == VM_BLE_BONDS) {
            _vm_ble_bonds(q);
        } else if (q->kind == VM_BLE_ERASE) {
            _vm_ble_erase(q);
        } else if (q->kind == VM_AFTER_RUN) {
            after_run_json(&q->out);
        } else if (q->kind == VM_FAULTS) {
            fault_json(&q->out);
        } else if (q->kind == VM_HEAP) {
            heap_json(&q->out);
        } else if (q->kind == VM_CLAIMED) {
            after_run_claimed_json(&q->out);
        } else if (q->kind == VM_AUDIO) {
            if (q->in != NULL && strcmp(q->in, "start") == 0) {
                audio_capture_start();
            } else if (q->in != NULL) {
                audio_capture_stop();
            }
            audio_capture_json(&q->out);
        } else if (q->kind == VM_AUDIO_WAV) {
            q->status = audio_capture_wav((size_t)atoi(q->in), &q->out) ? 200 : 404;
        }
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
    } else if (kind == VM_AUDIO_WAV && q->status == 404) {
        _error(r, 404, "no such recording");
    } else if (kind == VM_AUDIO_WAV) {
        _answer(r, 200, "audio/wav", q->out.data, q->out.len);
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

// ---- Bluetooth LE: the phone and the air ----

static void _buf_hex(api_buf_t *b, const uint8_t *data, size_t len) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        char two[2] = { digits[data[i] >> 4], digits[data[i] & 0xf] };
        api_buf_add(b, two, 2);
    }
}

// Hex text (spaces allowed) to bytes; -1 when it is not hex.
static int _unhex(const char *text, size_t text_len, uint8_t *out, size_t cap) {
    size_t n = 0;
    int high = -1;
    for (size_t i = 0; i < text_len; i++) {
        char c = text[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                continue;
            }
            return -1;
        }
        if (high < 0) {
            high = v;
        } else {
            if (n == cap) {
                return -1;
            }
            out[n++] = (uint8_t)(high << 4 | v);
            high = -1;
        }
    }
    return high < 0 ? (int)n : -1;
}

static void _ble_state(request_t *r) {
    api_buf_t b = {0};
    const char *controller = ble_host_controller();
    api_buf_printf(&b, "{\"controller\": \"%s\"", controller);
    if (strcmp(controller, "off") != 0) {
        ble_host_state_t s;
        ble_host_state(&s);
        char address[18];
        snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
            s.address[5], s.address[4], s.address[3], s.address[2], s.address[1], s.address[0]);
        api_buf_printf(&b, ", \"advertising\": %s, \"connectable\": %s, \"connected\": %s, \"scanning\": %s, "
            "\"public\": %s, \"address\": \"%s\", \"name\": ", s.advertising ? "true" : "false",
            s.connectable ? "true" : "false", s.connected ? "true" : "false", s.scanning ? "true" : "false",
            s.public ? "true" : "false", address);
        api_buf_json_string(&b, s.name, strlen(s.name));
        api_buf_add(&b, ", \"data\": \"", 11);
        _buf_hex(&b, s.data, s.data_len);
        api_buf_add(&b, "\", \"scan_response\": \"", 21);
        _buf_hex(&b, s.scan_response, s.scan_response_len);
        api_buf_add(&b, "\", \"links\": [", 13);
        const ble_host_link_t *encrypted = NULL;
        for (uint8_t i = 0; i < s.link_count; i++) {
            const ble_host_link_t *l = &s.links[i];
            api_buf_printf(&b, "%s{\"handle\": %u, \"peer\": \"%02x:%02x:%02x:%02x:%02x:%02x\", "
                "\"role\": \"%s\", \"encrypted\": %s}", i ? ", " : "", l->handle,
                l->peer[5], l->peer[4], l->peer[3], l->peer[2], l->peer[1], l->peer[0],
                l->central ? "central" : "peripheral", l->encrypted ? "true" : "false");
            if (l->encrypted && encrypted == NULL) {
                encrypted = l;
            }
        }
        // The first encrypted link's, and the bonds of the controller's identity; the BLE workflow.
        api_buf_printf(&b, "], \"encrypted\": %s, \"key_size\": %u, \"bonds\": %u",
            encrypted ? "true" : "false", encrypted ? encrypted->key_size : 0, s.bonds);
        api_buf_printf(&b, ", \"workflow\": \"%s\", \"bonded\": %s", ble_host_workflow_state(),
            ble_host_bonded() ? "true" : "false");
    }
    api_buf_add(&b, "}", 1);
    _answer_json(r, 200, &b);
    api_buf_free(&b);
}

// The phone's and the air's endpoints play the virtual controller's peers; with a real one
// they are real.
static bool _phone(request_t *r) {
    if (strcmp(ble_host_controller(), "virtual") == 0) {
        return true;
    }
    _error(r, 409, "no virtual controller: the phone is a real one, or there is no Bluetooth");
    return false;
}

#define CID_ATT 0x0004
#define CID_SMP 0x0006

// A PDU from the phone on channel cid (ATT, SMP), its body in hex.
static void _ble_pdu_send(request_t *r, uint16_t cid) {
    uint8_t pdu[512];
    int n = r->body == NULL ? -1 : _unhex(r->body, r->body_len, pdu, sizeof(pdu));
    if (n <= 0) {
        _error(r, 400, cid == CID_SMP ? "the body is an SMP PDU in hex" : "the body is an ATT PDU in hex");
    } else if (!ble_controller_l2cap_send(cid, pdu, (size_t)n)) {
        _error(r, 409, "no phone is connected");
    } else {
        _ok(r);
    }
}

// Every PDU the program has sent the phone on channel cid (ATT, SMP); with wait=MS, the first one
// waited for.
static void _ble_pdu_take(request_t *r, uint16_t cid) {
    long long wait = _param_int(r, "wait", 0);
    wait = wait < 0 ? 0 : wait > WAIT_MAX_MS ? WAIT_MAX_MS : wait;
    api_buf_t b = {0};
    api_buf_add(&b, "{\"pdus\": [", 10);
    uint8_t pdu[512];
    bool first = true;
    for (long long waited = 0;;) {
        size_t n = ble_controller_l2cap_take(cid, pdu, sizeof(pdu));
        if (n > 0) {
            api_buf_add(&b, first ? "\"" : ", \"", first ? 1 : 3);
            _buf_hex(&b, pdu, n);
            api_buf_add(&b, "\"", 1);
            first = false;
            continue;
        }
        if (!first || waited >= wait) {
            break;
        }
        struct timespec t = { 0, 5 * 1000 * 1000 };
        nanosleep(&t, NULL);
        waited += 5;
    }
    api_buf_add(&b, "]}", 2);
    _answer_json(r, 200, &b);
    api_buf_free(&b);
}

// "aa:bb:cc:dd:ee:ff" to bytes, least significant first; false when it is not an address.
static bool _address(const char *text, uint8_t out[6]) {
    unsigned v[6];
    char end;
    if (text == NULL || sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &end) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[5 - i] = (uint8_t)v[i];
    }
    return true;
}

static void _buf_address(api_buf_t *b, const char *key, const uint8_t a[6]) {
    api_buf_printf(b, ", \"%s\": \"%02x:%02x:%02x:%02x:%02x:%02x\"", key, a[5], a[4], a[3], a[2], a[1], a[0]);
}

static bool _body_has(request_t *r, const char *key) {
    char quoted[32];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    return r->body != NULL && strstr(r->body, quoted) != NULL;
}

// The body's "address" (and "random"), as bytes; false (and 400 answered) when there is none.
static bool _body_address(request_t *r, uint8_t address[6], bool *random) {
    char *text = NULL;
    double num = 0;
    bool ok = r->body != NULL && _json_field(r->body, "address", &text, NULL) && _address(text, address);
    free(text);
    if (!ok) {
        _error(r, 400, "the body needs \"address\": \"aa:bb:cc:dd:ee:ff\"");
        return false;
    }
    if (random != NULL) {
        *random = !_json_field(r->body, "random", NULL, &num) || num > (double)0.5;
    }
    return true;
}

// A hex field of the body into out, up to cap bytes: its length, 0 when absent, -1 when bad.
static int _body_hex(request_t *r, const char *key, uint8_t *out, size_t cap) {
    char *text = NULL;
    if (!_json_field(r->body, key, &text, NULL)) {
        return 0;
    }
    int n = _unhex(text, strlen(text), out, cap);
    free(text);
    return n;
}

static bool _body_flag(request_t *r, const char *key, bool fallback) {
    double num;
    return _json_field(r->body, key, NULL, &num) ? num > (double)0.5 : fallback;
}

static void _ble_peer(request_t *r) {
    ble_peer_t p = { .advertising = true, .connectable = true, .random = true, .rssi = -60 };
    if (!_body_address(r, p.address, &p.random)) {
        return;
    }
    double num;
    int data = _body_hex(r, "data", p.data, sizeof(p.data));
    int scan_response = _body_hex(r, "scan_response", p.scan_response, sizeof(p.scan_response));
    if (data < 0 || scan_response < 0) {
        _error(r, 400, "data and scan_response are hex, 31 bytes at most");
        return;
    }
    p.data_len = (uint8_t)data;
    p.scan_response_len = (uint8_t)scan_response;
    p.connectable = _body_flag(r, "connectable", true);
    p.advertising = _body_flag(r, "advertising", true);
    p.gone = _body_flag(r, "gone", false);
    if (_json_field(r->body, "rssi", NULL, &num)) {
        p.rssi = (int8_t)(num < -127 ? -127 : num > 20 ? 20 : num);
    }
    if (ble_controller_peer(&p) != 0) {
        _error(r, 409, "the air is full: 16 peers at most");
    } else {
        _ok(r);
    }
}

// {"address", "cid", "data"}: a frame from that peer to the program; without "address", from the
// phone.
static void _ble_l2cap(request_t *r) {
    uint8_t address[6];
    bool peer = _body_has(r, "address");
    if (peer && !_body_address(r, address, NULL)) {
        return;
    }
    uint8_t data[512];
    double cid = 0;
    int n = r->body == NULL ? -1 : _body_hex(r, "data", data, sizeof(data));
    if (n <= 0 || !_json_field(r->body, "cid", NULL, &cid) || cid < 1 || cid > 0xffff) {
        _error(r, 400, "the body needs \"cid\" and \"data\", an L2CAP frame's payload in hex");
    } else if (peer && ble_controller_l2cap(address, (uint16_t)cid, data, (size_t)n) != 0) {
        _error(r, 409, "no link to that address");
    } else if (!peer && !ble_controller_l2cap_send((uint16_t)cid, data, (size_t)n)) {
        _error(r, 409, "no phone is connected");
    } else {
        _ok(r);
    }
}

static const char *const _event_kinds[] = { "adv", "scan", "connected", "disconnected", "l2cap" };

// The air's events from since; with wait=MS, the first one waited for.
static void _ble_events(request_t *r) {
    long long since = _param_int(r, "since", 0);
    long long wait = _param_int(r, "wait", 0);
    wait = wait < 0 ? 0 : wait > WAIT_MAX_MS ? WAIT_MAX_MS : wait;
    ble_event_t *events = malloc(64 * sizeof(ble_event_t));
    if (events == NULL) {
        _error(r, 503, "out of memory");
        return;
    }
    uint64_t next;
    bool lost;
    size_t n = ble_controller_events(since < 0 ? 0 : (uint64_t)since, (int)wait, events, 64, &next, &lost);
    api_buf_t b = {0};
    char num[24];
    api_buf_printf(&b, "{\"next\": %s, \"lost\": %s, \"events\": [", _u64(num, next), lost ? "true" : "false");
    for (size_t i = 0; i < n; i++) {
        const ble_event_t *e = &events[i];
        api_buf_printf(&b, "%s{\"seq\": %s, \"kind\": \"%s\"", i ? ", " : "", _u64(num, e->seq), _event_kinds[e->kind]);
        switch (e->kind) {
            case BLE_EVENT_ADV:
                api_buf_printf(&b, ", \"on\": %s, \"connectable\": %s, \"random\": %s", e->on ? "true" : "false",
                    e->connectable ? "true" : "false", e->address_random ? "true" : "false");
                _buf_address(&b, "address", e->address);
                api_buf_add(&b, ", \"data\": \"", 11);
                _buf_hex(&b, e->data, e->len);
                api_buf_add(&b, "\", \"scan_response\": \"", 21);
                _buf_hex(&b, e->data + 31, e->scan_response_len);
                api_buf_add(&b, "\"", 1);
                break;
            case BLE_EVENT_SCAN:
                api_buf_printf(&b, ", \"on\": %s, \"active\": %s", e->on ? "true" : "false", e->active ? "true" : "false");
                break;
            default:
                _buf_address(&b, "peer", e->address);
                api_buf_printf(&b, ", \"peer_random\": %s, \"role\": \"%s\", \"handle\": %u", e->address_random ? "true" : "false",
                    e->role == 0 ? "central" : "peripheral", e->handle);
                if (e->kind == BLE_EVENT_CONNECTED) {
                    _buf_address(&b, "local", e->local);
                    api_buf_printf(&b, ", \"local_random\": %s, \"encrypted\": %s", e->local_random ? "true" : "false",
                        e->encrypted ? "true" : "false");
                } else if (e->kind == BLE_EVENT_DISCONNECTED) {
                    api_buf_printf(&b, ", \"reason\": %u", e->reason);
                } else {
                    api_buf_printf(&b, ", \"cid\": %u, \"data\": \"", e->cid);
                    _buf_hex(&b, e->data, e->len);
                    api_buf_add(&b, "\"", 1);
                }
                break;
        }
        api_buf_add(&b, "}", 1);
    }
    api_buf_add(&b, "]}", 2);
    free(events);
    _answer_json(r, 200, &b);
    api_buf_free(&b);
}

// Bytes given most significant first as the Bluetooth Core prints them, least significant first
// as SMP takes them.
static void _le_from(uint8_t *out, const char *hex, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * (n - 1 - i), "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void _buf_hex_msb(api_buf_t *b, const uint8_t *le, size_t n) {
    for (size_t i = n; i > 0; i--) {
        api_buf_printf(b, "%02x", le[i - 1]);
    }
}

// The host's c1, s1 and ah (devices/ble_hci smp_crypto.c) on the sample data of the Bluetooth
// Core (Vol 3 Part H 2.2.3, 2.2.4 and Appendix D.7): a check of the crypto the pairing uses.
static void _ble_crypto(request_t *r) {
    uint8_t k[16] = { 0 }, rnd[16], preq[7], prsp[7], ia[6], ra[6], r1[16], r2[16], irk[16], prand[3];
    uint8_t c1[16], s1[16], ah[3];
    _le_from(rnd, "5783D52156AD6F0E6388274EC6702EE0", 16);
    _le_from(preq, "07071000000101", 7);
    _le_from(prsp, "05000800000302", 7);
    _le_from(ia, "A1A2A3A4A5A6", 6);
    _le_from(ra, "B1B2B3B4B5B6", 6);
    smp_c1(k, rnd, preq, prsp, 1, ia, 0, ra, c1);
    _le_from(r1, "000F0E0D0C0B0A091122334455667788", 16);
    _le_from(r2, "010203040506070899AABBCCDDEEFF00", 16);
    smp_s1(k, r1, r2, s1);
    _le_from(irk, "EC0234A357C8AD05341010A60A397D9B", 16);
    _le_from(prand, "708194", 3);
    smp_ah(irk, prand, ah);
    api_buf_t b = {0};
    api_buf_add(&b, "{\"c1\": \"", 8);
    _buf_hex_msb(&b, c1, sizeof(c1));
    api_buf_add(&b, "\", \"s1\": \"", 10);
    _buf_hex_msb(&b, s1, sizeof(s1));
    api_buf_add(&b, "\", \"ah\": \"", 10);
    _buf_hex_msb(&b, ah, sizeof(ah));
    api_buf_add(&b, "\"}", 2);
    _answer_json(r, 200, &b);
    api_buf_free(&b);
}

// {"ltk": hex, "ediv": n, "rand": hex}: the phone encrypts its link (with "address", that peer
// its own); the host's answer waited for.
static void _ble_encrypt(request_t *r) {
    uint8_t ltk[16], rand[8], address[6];
    double ediv;
    char *ltk_hex = NULL, *rand_hex = NULL;
    bool peer = _body_has(r, "address");
    if (peer && !_body_address(r, address, NULL)) {
        return;
    }
    bool ok = r->body != NULL && _json_field(r->body, "ltk", &ltk_hex, NULL) &&
        _json_field(r->body, "rand", &rand_hex, NULL) && _json_field(r->body, "ediv", NULL, &ediv) &&
        ediv >= 0 && ediv <= 0xffff && _unhex(ltk_hex, strlen(ltk_hex), ltk, sizeof(ltk)) == 16 &&
        _unhex(rand_hex, strlen(rand_hex), rand, sizeof(rand)) == 8;
    free(ltk_hex);
    free(rand_hex);
    if (!ok) {
        _error(r, 400, "the body is {\"ltk\": 16 bytes hex, \"ediv\": n, \"rand\": 8 bytes hex}");
        return;
    }
    int link = ble_controller_encrypt(peer ? address : NULL, ltk, (uint16_t)ediv, rand);
    if (link < 0) {
        _error(r, 409, peer ? "no link to that address" : "no phone is connected");
        return;
    }
    int status = -1;
    for (int waited = 0; waited < 5000 && (status = ble_controller_encrypt_result(link)) < 0; waited += 5) {
        struct timespec t = { 0, 5 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    if (status < 0) {
        _error(r, 503, "the program's host did not answer the key request");
        return;
    }
    api_buf_t b = {0};
    api_buf_printf(&b, "{\"status\": %d}", status);
    _answer_json(r, 200, &b);
    api_buf_free(&b);
}

// {"address": "aa:bb:cc:dd:ee:ff", "type": "random"}: the phone's address for a connection.
static bool _phone_address(request_t *r, uint8_t address[6], uint8_t *type) {
    char *kind = NULL;
    if (!_body_address(r, address, NULL)) {
        return false;
    }
    bool ok = _json_field(r->body, "type", &kind, NULL) && (strcmp(kind, "random") == 0 || strcmp(kind, "public") == 0);
    *type = ok && strcmp(kind, "public") == 0 ? 0 : 1;
    free(kind);
    if (!ok) {
        _error(r, 400, "the phone's \"type\" is \"random\" or \"public\"");
    }
    return ok;
}

static void _ble(request_t *r, bool get, bool post) {
    if (strcmp(r->path, "/api/ble") == 0 && get) {
        _ble_state(r);
    } else if (strcmp(r->path, "/api/ble/crypto") == 0 && get) {
        _ble_crypto(r);
    } else if (strcmp(r->path, "/api/ble/bonds") == 0 && get) {
        _vm_call(r, VM_BLE_BONDS, _param_int(r, "keys", 0) ? "1" : "0");
    } else if (strcmp(r->path, "/api/ble/bonds/erase") == 0 && post) {
        _vm_call(r, VM_BLE_ERASE, NULL);
    } else if (strcmp(r->path, "/api/ble/connect") == 0 && post) {
        uint8_t address[6], type = 1;
        bool random;
        if (!_phone(r)) {
            return;
        }
        if (_body_has(r, "address") && !_body_has(r, "type")) {
            // A peer of that address connects, as a central.
            if (!_body_address(r, address, &random)) {
                return;
            }
            if (ble_controller_peer_connect(address, random) != 0) {
                _error(r, 409, "the program advertises nothing connectable, or all links are in use");
            } else {
                _ok(r);
            }
        } else if (_body_has(r, "address") && !_phone_address(r, address, &type)) {
            return;
        } else if (ble_controller_connect(_body_has(r, "address") ? address : NULL, type) != 0) {
            // The phone, from its own address or (with "type") from that one.
            _error(r, 409, "the program advertises nothing connectable, or a phone is connected");
        } else {
            _ok(r);
        }
    } else if (strcmp(r->path, "/api/ble/discover") == 0 && post) {
        // As reset plus the boot button on a board: answered first, then the lab resets.
        if (strcmp(ble_host_workflow_mode(), "off") == 0 || strcmp(ble_host_controller(), "off") == 0) {
            _error(r, 409, "the BLE workflow is off (--ble-workflow on or discover turns it on)");
        } else {
            _ok(r);
            ble_host_request_discovery();
        }
    } else if (strcmp(r->path, "/api/ble/disconnect") == 0 && post) {
        uint8_t address[6];
        if (!_phone(r)) {
            return;
        }
        // 0x13: the remote user ended it, as a phone that leaves does.
        if (_body_has(r, "address")) {
            if (!_body_address(r, address, NULL)) {
                return;
            }
            if (ble_controller_peer_disconnect(address, 0x13) != 0) {
                _error(r, 409, "no link to that address");
            } else {
                _ok(r);
            }
        } else {
            ble_controller_disconnect(0x13);
            _ok(r);
        }
    } else if (strcmp(r->path, "/api/ble/peer") == 0 && post) {
        if (_phone(r)) {
            _ble_peer(r);
        }
    } else if (strcmp(r->path, "/api/ble/l2cap") == 0 && post) {
        if (_phone(r)) {
            _ble_l2cap(r);
        }
    } else if (strcmp(r->path, "/api/ble/events") == 0 && get) {
        if (_phone(r)) {
            _ble_events(r);
        }
    } else if (strcmp(r->path, "/api/ble/att") == 0 && post) {
        if (_phone(r)) {
            _ble_pdu_send(r, CID_ATT);
        }
    } else if (strcmp(r->path, "/api/ble/att") == 0 && get) {
        if (_phone(r)) {
            _ble_pdu_take(r, CID_ATT);
        }
    } else if (strcmp(r->path, "/api/ble/smp") == 0 && post) {
        if (_phone(r)) {
            _ble_pdu_send(r, CID_SMP);
        }
    } else if (strcmp(r->path, "/api/ble/smp") == 0 && get) {
        if (_phone(r)) {
            _ble_pdu_take(r, CID_SMP);
        }
    } else if (strcmp(r->path, "/api/ble/encrypt") == 0 && post) {
        if (_phone(r)) {
            _ble_encrypt(r);
        }
    } else {
        _error(r, 404, "no such endpoint");
    }
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
        _stream(r, bus_trace_kept());
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
    } else if (strcmp(r->path, "/api/audio") == 0 && get) {
        _vm_call(r, VM_AUDIO, NULL);
    } else if ((strcmp(r->path, "/api/audio/start") == 0 || strcmp(r->path, "/api/audio/stop") == 0) && post) {
        _vm_call(r, VM_AUDIO, r->path + strlen("/api/audio/"));
    } else if (strcmp(r->path, "/api/audio/wav") == 0 && get) {
        char index[16];
        if (!_param(r, "index", index, sizeof(index)) || index[0] == '\0') {
            strcpy(index, "0");
        }
        _vm_call(r, VM_AUDIO_WAV, index);
    } else if (strcmp(r->path, "/api/after-run") == 0 && get) {
        _vm_call(r, VM_AFTER_RUN, NULL);
    } else if (strcmp(r->path, "/api/faults") == 0 && get) {
        _vm_call(r, VM_FAULTS, NULL);
    } else if (strcmp(r->path, "/api/heap") == 0 && get) {
        _vm_call(r, VM_HEAP, NULL);
    } else if (strcmp(r->path, "/api/claimed") == 0 && get) {
        _vm_call(r, VM_CLAIMED, NULL);
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
    } else if (strncmp(r->path, "/api/ble", 8) == 0) {
        _ble(r, get, post);
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
            // A client gone before its answer: EPIPE, not SIGPIPE ending the program.
            int one = 1;
            setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
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
    if (!api_ring_init(&_console, RING_SIZE) || !bus_trace_keep()) {
        fprintf(stderr, "circuitpython: api: out of memory\n");
        return false;
    }
    _on = true;

    // A process started by a reset takes the port the one before it had, also for --api 0.
    const char *again = getenv(RESTART_PORT_ENV);
    int port = again != NULL && again[0] != '\0' ? atoi(again) : desktop_options.api_port;
    platform_set_env(RESTART_PORT_ENV, NULL);
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
        platform_set_env(RESTART_PORT_ENV, port);
        platform_set_env("CIRCUITPY_API_TOKEN", _token);
    }
}
