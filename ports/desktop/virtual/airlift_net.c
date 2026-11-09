// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "airlift_net.h"

// Winsock before anything that brings windows.h in (pthread.h).
#include "host_socket.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/mpconfig.h"
#include "py/mphal.h"
#include "py/mpprint.h"

#include "bus_trace.h"
#include "host_net.h"

#if CIRCUITPY_SSL_MBEDTLS
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "lib/mbedtls_config/crt_bundle.h"
#endif

#define RX_MAX 8192
#define TX_MAX 2048
#define CONNECT_MS 5000
#define HANDSHAKE_MS 9000
#define WRITE_MS 5000

typedef struct {
    int mode;                   // -1: free
    int fd;
    bool listening, was_connected, eof;
    uint32_t remote_ip;         // network order
    uint16_t remote_port;
    uint8_t rx[RX_MAX];
    size_t rx_start, rx_end;
    uint8_t tx[TX_MAX];         // a UDP packet being put together
    size_t tx_len;
    char host[256];
    #if CIRCUITPY_SSL_MBEDTLS
    bool tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    #endif
} sock_t;

static sock_t _socks[AIRLIFT_SOCKETS];
static bool _ready;

typedef enum { JOB_RESOLVE, JOB_CONNECT, JOB_PING } kind_t;

typedef struct {
    kind_t kind;
    char host[256];
    uint32_t ip;
    uint16_t port;
    bool stream;
    uint8_t ttl;
} job_t;

// The worker's one job, under its lock; a result of an earlier generation (before a reset) is
// dropped by the worker itself.
static struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool started;
    enum { FREE, QUEUED, RUNNING, DONE } state;
    unsigned generation, job_generation;
    job_t job;
    int result, fd;
    uint32_t address;
} _w = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

// The job the command in progress waits for, on the VM thread.
static struct {
    bool waiting, queued;
    job_t job;
    uint8_t sock, mode;
} _want;

static int _handshaking = -1;
static uint32_t _handshake_start;
static uint32_t _resolved;

static void _init(void) {
    if (!_ready) {
        _ready = true;
        for (int i = 0; i < AIRLIFT_SOCKETS; i++) {
            _socks[i].mode = -1;
            _socks[i].fd = -1;
        }
    }
}

static sock_t *_sock(uint8_t n) {
    _init();
    return n < AIRLIFT_SOCKETS ? &_socks[n] : NULL;
}

static size_t _buffered(const sock_t *s) {
    return s->rx_end - s->rx_start;
}

static const char *_ip_text(uint32_t ip) {
    static char text[16];
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

static const char *_mode_name(int mode) {
    return mode == AIRLIFT_UDP ? "UDP" : mode == AIRLIFT_TLS ? "TLS" : "TCP";
}

#if CIRCUITPY_SSL_MBEDTLS
static void _tls_free(sock_t *s) {
    if (s->tls) {
        mbedtls_ssl_free(&s->ssl);
        mbedtls_ssl_config_free(&s->conf);
        mbedtls_x509_crt_free(&s->ca);
        s->tls = false;
    }
}
#endif

static void _close(uint8_t n, bool say) {
    sock_t *s = &_socks[n];
    bool open = s->mode >= 0 || s->fd >= 0;
    #if CIRCUITPY_SSL_MBEDTLS
    _tls_free(s);
    #endif
    if (s->fd >= 0) {
        host_sock_close(s->fd);
    }
    s->fd = -1;
    s->mode = -1;
    s->listening = s->was_connected = s->eof = false;
    s->rx_start = s->rx_end = s->tx_len = 0;
    s->remote_ip = 0;
    s->remote_port = 0;
    if (open && say) {
        bus_trace_text("AIRLIFT socket %u closed", n);
    }
}

// The worker
static uint32_t _lookup(const char *host) {
    struct addrinfo hints = { 0 }, *found = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &found) != 0 || found == NULL) {
        return 0;
    }
    uint32_t address = ((struct sockaddr_in *)found->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(found);
    return address;
}

static int _connect(uint32_t ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    host_sock_nosigpipe(fd);
    host_sock_set_nonblocking(fd);
    struct sockaddr_in to = { 0 };
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = ip;
    to.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&to, sizeof(to)) != 0) {
        int err = host_sock_errno();
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int error = 0;
        socklen_t len = sizeof(error);
        if ((err != EINPROGRESS && err != EAGAIN)
            || host_poll(&p, 1, CONNECT_MS) <= 0
            || (p.revents & (POLLERR | POLLHUP))
            || getsockopt(fd, SOL_SOCKET, SO_ERROR, HOST_SOCK_BUF(&error), &len) != 0
            || error != 0) {
            host_sock_close(fd);
            return -1;
        }
    }
    return fd;
}

static void *_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&_w.lock);
    for (;;) {
        while (_w.state != QUEUED) {
            pthread_cond_wait(&_w.wake, &_w.lock);
        }
        _w.state = RUNNING;
        job_t job = _w.job;
        unsigned generation = _w.job_generation;
        pthread_mutex_unlock(&_w.lock);

        int result, fd = -1;
        uint32_t address = job.ip;
        if (job.kind == JOB_PING) {
            result = host_ping_ms(job.ip, 1000);
        } else {
            if (job.host[0] != '\0') {
                address = _lookup(job.host);
            }
            result = address != 0;
            if (job.kind == JOB_CONNECT && job.stream && address != 0) {
                fd = _connect(address, job.port);
                result = fd >= 0;
            }
        }

        pthread_mutex_lock(&_w.lock);
        if (generation != _w.generation) {
            if (fd >= 0) {
                host_sock_close(fd);
            }
            _w.state = FREE;
        } else {
            _w.result = result;
            _w.fd = fd;
            _w.address = address;
            _w.state = DONE;
        }
    }
    return NULL;
}

static void _try_queue(void) {
    pthread_mutex_lock(&_w.lock);
    if (!_w.started) {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        _w.started = pthread_create(&thread, &attr, _worker, NULL) == 0;
        pthread_attr_destroy(&attr);
    }
    if (_w.started && _w.state == FREE) {
        _w.job = _want.job;
        _w.job_generation = _w.generation;
        _w.state = QUEUED;
        _want.queued = true;
        pthread_cond_signal(&_w.wake);
    }
    pthread_mutex_unlock(&_w.lock);
}

static void _ask(kind_t kind, const char *host, uint32_t ip, uint16_t port, bool stream, uint8_t ttl) {
    memset(&_want.job, 0, sizeof(_want.job));
    _want.job.kind = kind;
    snprintf(_want.job.host, sizeof(_want.job.host), "%s", host);
    _want.job.ip = ip;
    _want.job.port = port;
    _want.job.stream = stream;
    _want.job.ttl = ttl;
    _want.waiting = true;
    _want.queued = false;
    _try_queue();
}

// TLS on the VM thread
#if CIRCUITPY_SSL_MBEDTLS
static int _bio_send(void *ctx, const unsigned char *buf, size_t len) {
    sock_t *s = ctx;
    int r = (int)send(s->fd, HOST_SOCK_CBUF(buf), len, MSG_NOSIGNAL);
    if (r < 0) {
        return host_sock_errno() == EAGAIN ? MBEDTLS_ERR_SSL_WANT_WRITE : -1;
    }
    return r;
}

static int _bio_recv(void *ctx, unsigned char *buf, size_t len) {
    sock_t *s = ctx;
    int r = (int)recv(s->fd, HOST_SOCK_BUF(buf), len, 0);
    if (r < 0) {
        return host_sock_errno() == EAGAIN ? MBEDTLS_ERR_SSL_WANT_READ : -1;
    }
    return r;
}

// The bundle ssl trusts, or (for tests) the CA in the file CIRCUITPY_AIRLIFT_CAFILE names.
static bool _trust(sock_t *s) {
    const char *path = getenv("CIRCUITPY_AIRLIFT_CAFILE");
    if (path == NULL || path[0] == '\0') {
        return crt_bundle_attach(&s->conf) == 0;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "circuitpython: AirLift: CIRCUITPY_AIRLIFT_CAFILE %s: cannot read it\n", path);
        return false;
    }
    static unsigned char pem[16384];
    size_t len = fread(pem, 1, sizeof(pem) - 1, f);
    fclose(f);
    pem[len] = '\0';
    if (mbedtls_x509_crt_parse(&s->ca, pem, len + 1) != 0) {
        fprintf(stderr, "circuitpython: AirLift: CIRCUITPY_AIRLIFT_CAFILE %s: no certificate in it\n", path);
        return false;
    }
    mbedtls_ssl_conf_ca_chain(&s->conf, &s->ca, NULL);
    return true;
}

static bool _tls_begin(sock_t *s) {
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_x509_crt_init(&s->ca);
    s->tls = true;
    if (mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return false;
    }
    mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    if (!_trust(s) || mbedtls_ssl_setup(&s->ssl, &s->conf) != 0) {
        return false;
    }
    const char *name = s->host[0] != '\0' ? s->host : _ip_text(s->remote_ip);
    if (mbedtls_ssl_set_hostname(&s->ssl, name) != 0) {
        return false;
    }
    mbedtls_ssl_set_bio(&s->ssl, s, _bio_send, _bio_recv, NULL);
    return true;
}

// 1 done, 0 not yet, -1 failed (said in the trace).
static int _tls_step(uint8_t n) {
    sock_t *s = &_socks[n];
    int r = mbedtls_ssl_handshake(&s->ssl);
    if (r == 0) {
        return 1;
    }
    bool waiting = r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE;
    if (waiting && mp_hal_ticks_ms() - _handshake_start < HANDSHAKE_MS) {
        return 0;
    }
    uint32_t flags = mbedtls_ssl_get_verify_result(&s->ssl);
    // The bundle's verify callback ends a handshake with X509_FATAL_ERROR on a root it lacks.
    if ((flags != 0 && flags != (uint32_t)-1) || r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
        || r == MBEDTLS_ERR_X509_FATAL_ERROR) {
        bus_trace_text("AIRLIFT socket %u TLS %s failed: certificate not trusted (0x%x)", n, s->host, (unsigned)flags);
    } else if (waiting) {
        bus_trace_text("AIRLIFT socket %u TLS %s failed: no answer", n, s->host);
    } else {
        bus_trace_text("AIRLIFT socket %u TLS %s failed: -0x%04x", n, s->host, (unsigned)-r);
    }
    return -1;
}
#endif

// What a finished job means for the command waiting on it.
static int _done(int result, int fd, uint32_t address) {
    job_t *job = &_want.job;
    if (job->kind == JOB_RESOLVE) {
        _resolved = result ? address : 0;
        return result;
    }
    if (job->kind == JOB_PING) {
        return result;
    }
    uint8_t n = _want.sock;
    sock_t *s = &_socks[n];
    if (!result) {
        if (fd >= 0) {
            host_sock_close(fd);
        }
        bus_trace_text("AIRLIFT socket %u %s %s:%u failed", n, _mode_name(_want.mode),
            job->host[0] ? job->host : _ip_text(job->ip), job->port);
        return 0;
    }
    _close(n, false);
    s->fd = fd;
    s->remote_ip = address;
    s->remote_port = job->port;
    snprintf(s->host, sizeof(s->host), "%s", job->host);
    const char *name = job->host[0] ? job->host : _ip_text(address);
    if (_want.mode == AIRLIFT_TLS) {
        #if CIRCUITPY_SSL_MBEDTLS
        if (_tls_begin(s)) {
            _handshaking = n;
            _handshake_start = mp_hal_ticks_ms();
            return -2;
        }
        bus_trace_text("AIRLIFT socket %u TLS %s failed: no TLS set up", n, name);
        #endif
        _close(n, false);
        return 0;
    }
    s->mode = _want.mode;
    s->was_connected = _want.mode != AIRLIFT_UDP;
    bus_trace_text("AIRLIFT socket %u %s %s:%u", n, _mode_name(_want.mode), name, job->port);
    return 1;
}

bool airlift_net_poll(int *result) {
    #if CIRCUITPY_SSL_MBEDTLS
    if (_handshaking >= 0) {
        uint8_t n = (uint8_t)_handshaking;
        int r = _tls_step(n);
        if (r == 0) {
            return false;
        }
        _handshaking = -1;
        _want.waiting = false;
        sock_t *s = &_socks[n];
        if (r < 0) {
            _close(n, false);
            *result = 0;
            return true;
        }
        s->mode = AIRLIFT_TLS;
        s->was_connected = true;
        bus_trace_text("AIRLIFT socket %u TLS %s:%u", n, s->host[0] ? s->host : _ip_text(s->remote_ip), s->remote_port);
        *result = 1;
        return true;
    }
    #endif
    if (!_want.waiting) {
        *result = 0;
        return true;
    }
    if (!_want.queued) {
        _try_queue();
        return false;
    }
    pthread_mutex_lock(&_w.lock);
    bool done = _w.state == DONE;
    int r = _w.result, fd = _w.fd;
    uint32_t address = _w.address;
    if (done) {
        _w.state = FREE;
    }
    pthread_mutex_unlock(&_w.lock);
    if (!done) {
        return false;
    }
    r = _done(r, fd, address);
    if (r == -2) {
        return airlift_net_poll(result);    // the handshake's first step
    }
    _want.waiting = false;
    *result = r;
    return true;
}

void airlift_net_reset(void) {
    _init();
    for (uint8_t i = 0; i < AIRLIFT_SOCKETS; i++) {
        _close(i, false);
    }
    pthread_mutex_lock(&_w.lock);
    _w.generation++;
    if (_w.state == QUEUED) {
        _w.state = FREE;
    } else if (_w.state == DONE) {
        if (_w.fd >= 0) {
            host_sock_close(_w.fd);
        }
        _w.state = FREE;
    }
    pthread_mutex_unlock(&_w.lock);
    _want.waiting = false;
    _handshaking = -1;
    _resolved = 0;
}

uint8_t airlift_net_get_socket(void) {
    _init();
    for (uint8_t i = 0; i < AIRLIFT_SOCKETS; i++) {
        if (_socks[i].mode < 0 && !(_want.waiting && _want.sock == i)) {
            return i;
        }
    }
    return AIRLIFT_NO_SOCKET;
}

bool airlift_net_start(uint8_t n, const char *host, uint32_t ip, uint16_t port, uint8_t mode) {
    if (_sock(n) == NULL || mode > AIRLIFT_TLS || (host[0] == '\0' && ip == 0)) {
        return false;
    }
    #if !CIRCUITPY_SSL_MBEDTLS
    if (mode == AIRLIFT_TLS) {
        return false;
    }
    #endif
    _want.sock = n;
    _want.mode = mode;
    _ask(JOB_CONNECT, host, ip, port, mode != AIRLIFT_UDP, 0);
    return true;
}

void airlift_net_resolve(const char *host) {
    _ask(JOB_RESOLVE, host, 0, 0, false, 0);
}

void airlift_net_ping(uint32_t ip, uint8_t ttl) {
    _ask(JOB_PING, "", ip, 0, false, ttl);
}

uint32_t airlift_net_resolved(void) {
    return _resolved;
}

// What has come, into the socket's buffer: a TCP or TLS stream's bytes, or one UDP datagram.
static void _fill(sock_t *s) {
    if (s->fd < 0 || s->listening) {
        return;
    }
    if (s->mode == AIRLIFT_UDP) {
        if (_buffered(s) > 0) {
            return;
        }
        struct sockaddr_in from;
        socklen_t len = sizeof(from);
        int r = (int)recvfrom(s->fd, HOST_SOCK_BUF(s->rx), RX_MAX, 0, (struct sockaddr *)&from, &len);
        s->rx_start = 0;
        s->rx_end = r > 0 ? (size_t)r : 0;
        if (r > 0) {
            s->remote_ip = from.sin_addr.s_addr;
            s->remote_port = ntohs(from.sin_port);
        }
        return;
    }
    if (s->rx_start > 0) {
        memmove(s->rx, s->rx + s->rx_start, _buffered(s));
        s->rx_end -= s->rx_start;
        s->rx_start = 0;
    }
    while (!s->eof && s->rx_end < RX_MAX) {
        int r;
        #if CIRCUITPY_SSL_MBEDTLS
        if (s->mode == AIRLIFT_TLS) {
            r = mbedtls_ssl_read(&s->ssl, s->rx + s->rx_end, RX_MAX - s->rx_end);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                return;
            }
            if (r <= 0) {
                s->eof = true;
                return;
            }
            s->rx_end += (size_t)r;
            continue;
        }
        #endif
        r = (int)recv(s->fd, HOST_SOCK_BUF(s->rx + s->rx_end), RX_MAX - s->rx_end, 0);
        if (r > 0) {
            s->rx_end += (size_t)r;
        } else {
            s->eof = r == 0 || host_sock_errno() != EAGAIN;
            return;
        }
    }
}

uint8_t airlift_net_state(uint8_t n) {
    sock_t *s = _sock(n);
    if (s == NULL) {
        return 0;
    }
    if ((s->mode == AIRLIFT_TCP || s->mode == AIRLIFT_TLS) && !s->listening) {
        _fill(s);
        if (s->fd >= 0 && (!s->eof || _buffered(s) > 0)) {
            return 4;
        }
    }
    // A connection that has ended gives its slot back, as nina-fw does.
    if (s->was_connected) {
        _close(n, true);
    }
    return 0;
}

static uint16_t _accept(uint8_t n) {
    sock_t *s = &_socks[n];
    struct sockaddr_in from;
    socklen_t len = sizeof(from);
    int fd = (int)accept(s->fd, (struct sockaddr *)&from, &len);
    if (fd < 0) {
        return AIRLIFT_NO_SOCKET;
    }
    for (uint8_t i = 0; i < AIRLIFT_SOCKETS; i++) {
        sock_t *c = &_socks[i];
        if (i != n && c->mode < 0) {
            host_sock_nosigpipe(fd);
            host_sock_set_nonblocking(fd);
            _close(i, false);
            c->fd = fd;
            c->mode = AIRLIFT_TCP;
            c->was_connected = true;
            c->remote_ip = from.sin_addr.s_addr;
            c->remote_port = ntohs(from.sin_port);
            bus_trace_text("AIRLIFT socket %u from %s:%u", i, _ip_text(c->remote_ip), c->remote_port);
            return i;
        }
    }
    host_sock_close(fd);
    return AIRLIFT_NO_SOCKET;
}

uint16_t airlift_net_available(uint8_t n) {
    sock_t *s = _sock(n);
    if (s == NULL || s->mode < 0) {
        return 0;
    }
    if (s->listening && s->mode == AIRLIFT_TCP) {
        return _accept(n);
    }
    _fill(s);
    size_t have = _buffered(s);
    return have > 0xffff ? 0xffff : (uint16_t)have;
}

int airlift_net_read(uint8_t n, uint8_t *buf, uint16_t len, bool peek) {
    sock_t *s = _sock(n);
    if (s == NULL || s->mode < 0) {
        return -1;
    }
    if (_buffered(s) == 0) {
        _fill(s);
    }
    size_t have = _buffered(s);
    if (have == 0) {
        return -1;
    }
    size_t k = have < len ? have : len;
    memcpy(buf, s->rx + s->rx_start, k);
    if (!peek) {
        s->rx_start += k;
    }
    return (int)k;
}

uint16_t airlift_net_write(uint8_t n, const uint8_t *buf, uint16_t len) {
    sock_t *s = _sock(n);
    if (s == NULL || s->fd < 0 || s->listening || (s->mode != AIRLIFT_TCP && s->mode != AIRLIFT_TLS)) {
        return 0;
    }
    uint32_t start = mp_hal_ticks_ms();
    size_t done = 0;
    while (done < len && mp_hal_ticks_ms() - start < WRITE_MS) {
        short events = POLLOUT;
        int r;
        #if CIRCUITPY_SSL_MBEDTLS
        if (s->mode == AIRLIFT_TLS) {
            r = mbedtls_ssl_write(&s->ssl, buf + done, len - done);
            if (r >= 0) {
                done += (size_t)r;
                continue;
            }
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
                break;
            }
            events = r == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT;
        } else
        #endif
        {
            r = (int)send(s->fd, HOST_SOCK_CBUF(buf + done), len - done, MSG_NOSIGNAL);
            if (r > 0) {
                done += (size_t)r;
                continue;
            }
            if (r < 0 && host_sock_errno() != EAGAIN) {
                break;
            }
        }
        struct pollfd p = { .fd = s->fd, .events = events };
        host_poll(&p, 1, 100);
    }
    return (uint16_t)done;
}

void airlift_net_stop(uint8_t n) {
    if (_sock(n) != NULL) {
        if (_want.waiting && _want.sock == n && _handshaking == n) {
            _handshaking = -1;
            _want.waiting = false;
        }
        _close(n, true);
    }
}

static int _udp_socket(sock_t *s) {
    if (s->fd < 0) {
        s->fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
        if (s->fd >= 0) {
            host_sock_set_nonblocking(s->fd);
        }
    }
    return s->fd;
}

bool airlift_net_listen(uint8_t n, uint16_t port, uint8_t mode) {
    sock_t *s = _sock(n);
    if (s == NULL) {
        return false;
    }
    struct sockaddr_in at = { 0 };
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_ANY);
    at.sin_port = htons(port);
    if (mode == AIRLIFT_UDP) {
        // nina-fw binds the port it was given, which for a client is the server's: here a client
        // takes a port of its own (the server may be on this computer), as does a server whose
        // port is taken.
        if (_udp_socket(s) < 0) {
            return false;
        }
        if (s->remote_ip != 0 && s->remote_port == port) {
            at.sin_port = 0;
        }
        if (bind(s->fd, (struct sockaddr *)&at, sizeof(at)) != 0) {
            at.sin_port = 0;
            bind(s->fd, (struct sockaddr *)&at, sizeof(at));
        }
        s->mode = AIRLIFT_UDP;
        return true;
    }
    if (mode != AIRLIFT_TCP) {
        return false;
    }
    _close(n, false);
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (fd < 0) {
        return false;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, HOST_SOCK_CBUF(&one), sizeof(one));
    bool bound = bind(fd, (struct sockaddr *)&at, sizeof(at)) == 0;
    int err = bound ? 0 : host_sock_errno();
    // As wifi's sockets: a port the computer will not give (taken, below 1024 without root)
    // becomes 8080, 8081 ... or the ones after it.
    if (!bound && port != 0 && (err == EADDRINUSE || err == EACCES)) {
        uint32_t first = port < 8080 ? 8080 : port + 1u;
        for (uint32_t moved = first; !bound && moved < first + 100 && moved <= 65535; moved++) {
            at.sin_port = htons((uint16_t)moved);
            if (bind(fd, (struct sockaddr *)&at, sizeof(at)) == 0) {
                bound = true;
                mp_printf(&mp_plat_print, "port %u is %s, using %u\n", (unsigned)port,
                    err == EACCES ? "not allowed" : "in use", (unsigned)moved);
                port = (uint16_t)moved;
            }
        }
    }
    if (!bound || listen(fd, 4) != 0) {
        host_sock_close(fd);
        return false;
    }
    host_sock_set_nonblocking(fd);
    s->fd = fd;
    s->mode = AIRLIFT_TCP;
    s->listening = true;
    bus_trace_text("AIRLIFT socket %u listen %u", n, port);
    return true;
}

uint8_t airlift_net_server_state(uint8_t n) {
    sock_t *s = _sock(n);
    return s != NULL && s->listening;
}

bool airlift_net_udp_append(uint8_t n, const uint8_t *buf, uint16_t len) {
    sock_t *s = _sock(n);
    if (s == NULL || s->mode != AIRLIFT_UDP || len == 0) {
        return false;
    }
    size_t k = len < TX_MAX - s->tx_len ? len : TX_MAX - s->tx_len;
    memcpy(s->tx + s->tx_len, buf, k);
    s->tx_len += k;
    return k > 0;
}

bool airlift_net_udp_send(uint8_t n) {
    sock_t *s = _sock(n);
    if (s == NULL || s->mode != AIRLIFT_UDP || s->remote_ip == 0 || _udp_socket(s) < 0) {
        return false;
    }
    struct sockaddr_in to = { 0 };
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = s->remote_ip;
    to.sin_port = htons(s->remote_port);
    int r = (int)sendto(s->fd, HOST_SOCK_CBUF(s->tx), s->tx_len, 0, (struct sockaddr *)&to, sizeof(to));
    s->tx_len = 0;
    return r >= 0;
}

void airlift_net_remote(uint8_t n, uint32_t *ip, uint16_t *port) {
    sock_t *s = _sock(n);
    *ip = s != NULL ? s->remote_ip : 0;
    *port = s != NULL ? s->remote_port : 0;
}
