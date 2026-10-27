// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2020 Lucian Copeland for Adafruit Industries
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka (POSIX sockets)
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/socketpool/Socket.h"

#include "shared/runtime/interrupt_char.h"
#include "py/mperrno.h"
#include "py/runtime.h"
#include "shared-bindings/socketpool/SocketPool.h"
#include "common-hal/socketpool/__init__.h"
#if CIRCUITPY_SSL
#include "shared-bindings/ssl/SSLSocket.h"
#include "shared-module/ssl/SSLSocket.h"
#endif
#include "supervisor/port.h"
#include "supervisor/shared/tick.h"
#include "supervisor/workflow.h"

#include "desktop_port.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_socket.h"

#define SOCKETPOOL_IP_STR_LEN 48

static mp_obj_t _format_address(const struct sockaddr *addr, int family) {
    char ip_str[SOCKETPOOL_IP_STR_LEN];
    const struct sockaddr_in *a = (void *)addr;

    switch (family) {
        #if CIRCUITPY_SOCKETPOOL_IPV6
        case AF_INET6:
            inet_ntop(family, &((const struct sockaddr_in6 *)a)->sin6_addr, ip_str, sizeof(ip_str));
            break;
        #endif
        default:
        case AF_INET:
            inet_ntop(family, &((const struct sockaddr_in *)a)->sin_addr, ip_str, sizeof(ip_str));
            break;
    }
    return mp_obj_new_str(ip_str, strlen(ip_str));
}

// The length of the address its family has: BSD systems (macOS) refuse a longer one.
static socklen_t _sockaddr_len(const struct sockaddr_storage *addr) {
    return addr->ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
}

static mp_obj_t _sockaddr_to_tuple(const struct sockaddr_storage *sockaddr) {
    mp_obj_t args[4] = {
        _format_address((const struct sockaddr *)sockaddr, sockaddr->ss_family),
    };
    int n = 2;
    #if CIRCUITPY_SOCKETPOOL_IPV6
    if (sockaddr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *addr6 = (const void *)sockaddr;
        args[1] = MP_OBJ_NEW_SMALL_INT(ntohs(addr6->sin6_port));
        args[2] = MP_OBJ_NEW_SMALL_INT(addr6->sin6_flowinfo);
        args[3] = MP_OBJ_NEW_SMALL_INT(addr6->sin6_scope_id);
        n = 4;
    } else
    #endif
    {
        const struct sockaddr_in *addr = (const void *)sockaddr;
        args[1] = MP_OBJ_NEW_SMALL_INT(ntohs(addr->sin_port));
    }
    return mp_obj_new_tuple(n, args);
}

static void socketpool_resolve_host_or_throw(int family, int type, const char *hostname, struct sockaddr_storage *addr, int port) {
    const struct addrinfo hints = {
        .ai_family = family,
        .ai_socktype = type,
    };
    struct addrinfo *result_i = NULL;
    char service_buf[6];

    snprintf(service_buf, sizeof(service_buf), "%d", port);

    int error = getaddrinfo(hostname, service_buf, &hints, &result_i);
    if (error != 0 || result_i == NULL) {
        // Report what the resolver said. A null result with no error is the
        // one case that really is "not found".
        if (error == 0) {
            error = EAI_NONAME;
        }
        common_hal_socketpool_socketpool_raise_gaierror(error);
    }

    memcpy(addr, result_i->ai_addr, result_i->ai_addrlen);
    freeaddrinfo(result_i);
}

static void resolve_host_or_throw(socketpool_socket_obj_t *self, const char *hostname, struct sockaddr_storage *addr, int port) {
    socketpool_resolve_host_or_throw(self->family, self->type, hostname, addr, port);
}

// How long to wait between checks for a socket to connect.
#define SOCKET_CONNECT_POLL_INTERVAL_MS 100

// Wait up to 10 ms for the socket instead of retrying at once: on a host a tight retry loop
// would hold a core while the network is slow. A signal (the tick) only shortens the wait.
static void _wait_ready(int fd, short events) {
    struct pollfd p = { .fd = fd, .events = events };
    host_poll(&p, 1, 10);
}

void socket_user_reset(void) {
    // User sockets are heap objects with __del__ bound to close().
    // During VM shutdown/reset, gc_sweep_all() runs finalizers, so sockets
    // are closed there rather than being tracked and closed explicitly here.
}

// The workflow polls its sockets again shortly (a one-shot timer interrupt in supervisor/port.c).
void socketpool_socket_poll_resume(void) {
    desktop_request_workflow_poll(10);
}

static bool _socketpool_socket(socketpool_socketpool_obj_t *self,
    socketpool_socketpool_addressfamily_t family, socketpool_socketpool_sock_t type,
    int proto,
    socketpool_socket_obj_t *sock) {
    int addr_family = AF_INET;
    int ipproto = IPPROTO_IP;

    if (family == SOCKETPOOL_AF_INET) {
        addr_family = AF_INET;
        ipproto = IPPROTO_IP;
    #if CIRCUITPY_SOCKETPOOL_IPV6
    } else { // INET6
        addr_family = AF_INET6;
        ipproto = IPPROTO_IPV6;
    #endif
    }

    int socket_type;
    if (type == SOCKETPOOL_SOCK_STREAM) {
        socket_type = SOCK_STREAM;
    } else if (type == SOCKETPOOL_SOCK_DGRAM) {
        socket_type = SOCK_DGRAM;
    } else { // SOCKETPOOL_SOCK_RAW
        socket_type = SOCK_RAW;
        ipproto = proto;
    }
    sock->type = socket_type;
    sock->family = addr_family;
    sock->ipproto = ipproto;
    sock->pool = self;
    sock->timeout_ms = (uint)-1;

    int socknum = socket(sock->family, sock->type, sock->ipproto);
    if (socknum < 0) {
        return false;
    }

    sock->num = socknum;

    #ifndef _WIN32
    // Enable address reuse by default to avoid bind failures on recently-used ports. (Winsock's
    // SO_REUSEADDR lets a second socket take a port in use, and a closed port is free at once.)
    int reuseaddr = 1;
    if (setsockopt(socknum, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)) < 0) {
        // Ignore if SO_REUSEADDR is unsupported.
    }
    #endif

    // Sockets should be nonblocking in most cases.
    if (host_sock_set_nonblocking(socknum) < 0) {
        // Ignore if non-blocking is unsupported.
    }

    return true;
}

// special entry for workflow listener (register system socket)
bool socketpool_socket(socketpool_socketpool_obj_t *self,
    socketpool_socketpool_addressfamily_t family, socketpool_socketpool_sock_t type,
    int proto, socketpool_socket_obj_t *sock) {

    if (!_socketpool_socket(self, family, type, proto, sock)) {
        return false;
    }

    return true;
}

socketpool_socket_obj_t *common_hal_socketpool_socket(socketpool_socketpool_obj_t *self,
    socketpool_socketpool_addressfamily_t family, socketpool_socketpool_sock_t type, int proto) {
    switch (family) {
        #if CIRCUITPY_SOCKETPOOL_IPV6
        case SOCKETPOOL_AF_INET6:
        #endif
        case SOCKETPOOL_AF_INET:
            break;
        default:
            mp_raise_NotImplementedError(MP_ERROR_TEXT("Unsupported socket type"));
    }

    socketpool_socket_obj_t *sock = mp_obj_malloc_with_finaliser(socketpool_socket_obj_t, &socketpool_socket_type);

    if (!_socketpool_socket(self, family, type, proto, sock)) {
        // Report socket's real error: -EPFNOSUPPORT (e.g. TCP compiled
        // out) is very different from a full fd table (EMFILE), so hiding the
        // errno behind the generic "Out of sockets" hides the actual cause.
        mp_raise_OSError(host_sock_errno());
    }
    sock->user = true;
    return sock;
}

int socketpool_socket_accept(socketpool_socket_obj_t *self, mp_obj_t *peer_out, socketpool_socket_obj_t *accepted) {
    struct sockaddr_storage peer_addr;
    socklen_t socklen = sizeof(peer_addr);
    int newsoc = -1;
    bool timed_out = false;
    uint64_t start_ticks = supervisor_ticks_ms64();

    // Allow timeouts and interrupts
    while (newsoc == -1 && !timed_out) {
        if (self->timeout_ms != (uint)-1 && self->timeout_ms != 0) {
            timed_out = supervisor_ticks_ms64() - start_ticks >= self->timeout_ms;
        }
        RUN_BACKGROUND_TASKS;
        newsoc = accept(self->num, (struct sockaddr *)&peer_addr, &socklen);
        int err = host_sock_errno();
        // In non-blocking mode, fail instead of timing out
        if (newsoc == -1 && (self->timeout_ms == 0 || mp_hal_is_interrupted())) {
            return -MP_EAGAIN;
        }
        if (newsoc == -1 && err != EAGAIN && err != EWOULDBLOCK) {
            return -err;
        }
        if (newsoc == -1) {
            _wait_ready(self->num, POLLIN);
        }
    }

    if (timed_out) {
        return -ETIMEDOUT;
    }

    if (newsoc < 0) {
        return -MP_EBADF;
    }

    // We got a socket. New client socket will not be non-blocking by default, so make it non-blocking.
    if (host_sock_set_nonblocking(newsoc) < 0) {
        // Ignore if non-blocking is unsupported.
    }

    if (accepted != NULL) {
        // Error if called with open socket object.
        assert(common_hal_socketpool_socket_get_closed(accepted));

        // Replace the old accepted socket with the new one.
        accepted->num = newsoc;
        accepted->pool = self->pool;
        accepted->connected = true;
        accepted->type = self->type;
    }

    if (peer_out) {
        *peer_out = _sockaddr_to_tuple(&peer_addr);
    }

    return newsoc;
}

socketpool_socket_obj_t *common_hal_socketpool_socket_accept(socketpool_socket_obj_t *self, mp_obj_t *peer_out) {
    // Set the socket type only after the socketpool_socket_accept succeeds, so that the
    // finaliser is not called on a bad socket.
    socketpool_socket_obj_t *sock = mp_obj_malloc_with_finaliser(socketpool_socket_obj_t, NULL);
    int newsoc = socketpool_socket_accept(self, peer_out, NULL);

    if (newsoc > 0) {
        // Create the socket
        sock->base.type = &socketpool_socket_type;
        sock->num = newsoc;
        sock->pool = self->pool;
        sock->connected = true;
        sock->type = self->type;

        return sock;
    } else {
        mp_raise_OSError(-newsoc);
        return NULL;
    }
}

size_t common_hal_socketpool_socket_bind(socketpool_socket_obj_t *self,
    const char *host, size_t hostlen, uint32_t port) {
    struct sockaddr_storage bind_addr;
    const char *broadcast = "<broadcast>";
    uint32_t local_port = port;

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.ss_family = self->family;

    #if CIRCUITPY_SOCKETPOOL_IPV6
    if (self->family == AF_INET6) {
        struct sockaddr_in6 *addr6 = (void *)&bind_addr;
        addr6->sin6_port = htons(local_port);
        // no ipv6 broadcast
        if (hostlen == 0) {
            memset(&addr6->sin6_addr, 0, sizeof(addr6->sin6_addr));
        } else {
            socketpool_resolve_host_or_throw(self->family, self->type, host, &bind_addr, local_port);
        }
    } else
    #endif
    {
        struct sockaddr_in *addr4 = (void *)&bind_addr;
        addr4->sin_port = htons(local_port);
        if (hostlen == 0) {
            addr4->sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (hostlen == strlen(broadcast) &&
                   memcmp(host, broadcast, strlen(broadcast)) == 0) {
            addr4->sin_addr.s_addr = htonl(INADDR_BROADCAST);
        } else {
            socketpool_resolve_host_or_throw(self->family, self->type, host, &bind_addr, local_port);
        }
    }

    int result = bind(self->num, (struct sockaddr *)&bind_addr, _sockaddr_len(&bind_addr));
    if (result == 0) {
        return 0;
    }
    int err = host_sock_errno();
    // A program's server the computer will not give its port to (taken, or below 1024 without
    // root) takes 8080, 8081 ... or the ports after its own. UDP keeps its port: a protocol may
    // need it.
    if (!self->user || self->type != SOCK_STREAM || port == 0 || (err != EADDRINUSE && err != EACCES)) {
        return err;
    }
    uint32_t first = port < 8080 ? 8080 : port + 1;
    for (uint32_t moved = first; moved < first + 100 && moved <= 65535; moved++) {
        // The port lies at the same place in sockaddr_in and sockaddr_in6.
        ((struct sockaddr_in *)&bind_addr)->sin_port = htons(moved);
        if (bind(self->num, (struct sockaddr *)&bind_addr, _sockaddr_len(&bind_addr)) == 0) {
            mp_printf(&mp_plat_print, "port %u is %s, using %u\n", (unsigned)port,
                err == EACCES ? "not allowed" : "in use", (unsigned)moved);
            return 0;
        }
    }
    return err;
}

void socketpool_socket_close(socketpool_socket_obj_t *self) {
    #if CIRCUITPY_SSL
    if (self->ssl_socket) {
        ssl_sslsocket_obj_t *ssl_socket = self->ssl_socket;
        self->ssl_socket = NULL;
        common_hal_ssl_sslsocket_close(ssl_socket);
        return;
    }
    #endif
    self->connected = false;
    int fd = self->num;
    if (fd >= 0) {
        host_sock_close(fd);
    }
    self->num = -1;
}

void common_hal_socketpool_socket_close(socketpool_socket_obj_t *self) {
    socketpool_socket_close(self);
}

void common_hal_socketpool_socket_connect(socketpool_socket_obj_t *self,
    const char *host, size_t hostlen, uint32_t port) {
    (void)hostlen;
    struct sockaddr_storage addr;
    resolve_host_or_throw(self, host, &addr, port);

    int result = connect(self->num, (struct sockaddr *)&addr, _sockaddr_len(&addr));

    if (result == 0) {
        // Connected immediately.
        self->connected = true;
        return;
    }

    int err = host_sock_errno();
    if (result < 0 && err != EINPROGRESS && err != EAGAIN) {
        // Some error happened. (Winsock reports a connect under way as "would block".)
        mp_raise_OSError(err);
        return;
    }

    // Keep checking, using poll(), until timeout expires, at short intervals.
    // This allows ctrl-C interrupts to be detected and background tasks to run.
    mp_uint_t timeout_left = self->timeout_ms;

    while (timeout_left > 0) {
        RUN_BACKGROUND_TASKS;
        // Allow ctrl-C interrupt
        if (mp_hal_is_interrupted()) {
            return;
        }

        struct pollfd fd = {
            .fd = self->num,
            .events = POLLOUT,
        };
        int poll_timeout = SOCKET_CONNECT_POLL_INTERVAL_MS;
        if (self->timeout_ms != (uint)-1 && timeout_left < SOCKET_CONNECT_POLL_INTERVAL_MS) {
            poll_timeout = timeout_left;
        }

        result = host_poll(&fd, 1, poll_timeout);
        if (result == 0) {
            if (self->timeout_ms == (uint)-1) {
                continue;
            }
            if (timeout_left < SOCKET_CONNECT_POLL_INTERVAL_MS) {
                timeout_left = 0;
            } else {
                timeout_left -= SOCKET_CONNECT_POLL_INTERVAL_MS;
            }
            continue;
        }

        if (result < 0) {
            if (host_sock_errno() == EINTR) {
                continue;               // an interrupt signal, not an error
            }
            mp_raise_OSError(host_sock_errno());
        }

        int error_code = 0;
        socklen_t socklen = sizeof(error_code);
        result = getsockopt(self->num, SOL_SOCKET, SO_ERROR, HOST_SOCK_BUF(&error_code), &socklen);
        if (result < 0 || error_code != 0) {
            mp_raise_OSError(error_code != 0 ? host_sock_errno_of(error_code) : host_sock_errno());
        }
        self->connected = true;
        return;
    }

    // No connection after timeout. The connection attempt is not stopped.
    // This imitates what happens in Python.
    mp_raise_OSError(ETIMEDOUT);
}

bool common_hal_socketpool_socket_get_closed(socketpool_socket_obj_t *self) {
    return self->num < 0;
}

bool common_hal_socketpool_socket_get_connected(socketpool_socket_obj_t *self) {
    return self->connected;
}

bool common_hal_socketpool_socket_listen(socketpool_socket_obj_t *self, int backlog) {
    return listen(self->num, backlog) == 0;
}

mp_uint_t common_hal_socketpool_socket_recvfrom_into(socketpool_socket_obj_t *self,
    uint8_t *buf, uint32_t len, mp_obj_t *source_out) {

    struct sockaddr_storage source_addr;
    socklen_t socklen = sizeof(source_addr);

    uint64_t start_ticks = supervisor_ticks_ms64();
    int received = -1;
    bool timed_out = false;
    while (received == -1 &&
           !timed_out &&
           !mp_hal_is_interrupted()) {
        if (self->timeout_ms != (uint)-1 && self->timeout_ms != 0) {
            timed_out = supervisor_ticks_ms64() - start_ticks >= self->timeout_ms;
        }
        RUN_BACKGROUND_TASKS;
        received = recvfrom(self->num, HOST_SOCK_BUF(buf), len, MSG_DONTWAIT, (struct sockaddr *)&source_addr, &socklen);
        int err = host_sock_errno();

        if (received < 0 && err != EAGAIN && err != EWOULDBLOCK) {
            mp_raise_OSError(err);
        }

        if (received == -1 && (err == EAGAIN || err == EWOULDBLOCK)) {
            // In non-blocking mode, fail instead of looping
            if (self->timeout_ms == 0) {
                mp_raise_OSError(MP_EAGAIN);
            }
            _wait_ready(self->num, POLLIN);
            continue;
        }
    }

    if (timed_out) {
        mp_raise_OSError(ETIMEDOUT);
    }

    if (received < 0) {
        mp_raise_BrokenPipeError();
        return 0;
    }

    if (source_out) {
        *source_out = _sockaddr_to_tuple(&source_addr);
    }

    return received;
}

int socketpool_socket_recv_into(socketpool_socket_obj_t *self,
    const uint8_t *buf, uint32_t len) {
    int received = 0;
    bool timed_out = false;

    if (self->num != -1) {
        uint64_t start_ticks = supervisor_ticks_ms64();
        received = -1;
        while (received == -1 &&
               !timed_out) {
            if (self->timeout_ms != (uint)-1 && self->timeout_ms != 0) {
                timed_out = supervisor_ticks_ms64() - start_ticks >= self->timeout_ms;
            }
            RUN_BACKGROUND_TASKS;
            received = recv(self->num, HOST_SOCK_BUF(buf), len, MSG_DONTWAIT);
            int err = host_sock_errno();
            if (received < 0 && err != EAGAIN && err != EWOULDBLOCK) {
                return -err;
            }
            if (received < 1 && (err == EAGAIN || err == EWOULDBLOCK)) {
                if (self->timeout_ms == 0) {
                    return -MP_EAGAIN;
                }
                _wait_ready(self->num, POLLIN);
                continue;
            }
            // In non-blocking mode, fail instead of looping
            if (received < 1 && self->timeout_ms == 0) {
                if ((received == 0) || (err == ENOTCONN)) {
                    self->connected = false;
                    return -MP_ENOTCONN;
                }
                return -MP_EAGAIN;
            }
            // Check this after going through the loop once so it can make
            // progress while interrupted.
            if (mp_hal_is_interrupted()) {
                if (received == -1) {
                    return -MP_EAGAIN;
                }
                break;
            }
        }
    } else {
        return -MP_EBADF;
    }

    if (timed_out) {
        return -ETIMEDOUT;
    }
    return received;
}

mp_uint_t common_hal_socketpool_socket_recv_into(socketpool_socket_obj_t *self, const uint8_t *buf, uint32_t len) {
    int received = socketpool_socket_recv_into(self, buf, len);
    if (received < 0) {
        mp_raise_OSError(-received);
    }
    return received;
}

int socketpool_socket_send(socketpool_socket_obj_t *self, const uint8_t *buf, uint32_t len) {
    int sent = -1;
    if (self->num != -1) {
        sent = send(self->num, HOST_SOCK_CBUF(buf), len, MSG_NOSIGNAL);
    } else {
        sent = -MP_EBADF;
    }

    if (sent < 0) {
        int err = host_sock_errno();
        if (err == ECONNRESET || err == ENOTCONN) {
            self->connected = false;
        }
        return -err;
    }

    return sent;
}

mp_uint_t common_hal_socketpool_socket_send(socketpool_socket_obj_t *self, const uint8_t *buf, uint32_t len) {
    int sent = socketpool_socket_send(self, buf, len);

    if (sent < 0) {
        mp_raise_OSError(-sent);
    }
    return sent;
}

mp_uint_t common_hal_socketpool_socket_sendto(socketpool_socket_obj_t *self,
    const char *host, size_t hostlen, uint32_t port, const uint8_t *buf, uint32_t len) {

    (void)hostlen;
    struct sockaddr_storage addr;
    resolve_host_or_throw(self, host, &addr, port);

    int bytes_sent = sendto(self->num, HOST_SOCK_CBUF(buf), len, MSG_NOSIGNAL, (struct sockaddr *)&addr, _sockaddr_len(&addr));
    if (bytes_sent < 0) {
        mp_raise_BrokenPipeError();
        return 0;
    }
    return bytes_sent;
}

void common_hal_socketpool_socket_settimeout(socketpool_socket_obj_t *self, uint32_t timeout_ms) {
    self->timeout_ms = timeout_ms;
}

mp_int_t common_hal_socketpool_socket_get_type(socketpool_socket_obj_t *self) {
    return self->type;
}


int common_hal_socketpool_socket_setsockopt(socketpool_socket_obj_t *self, int level, int optname, const void *value, size_t optlen) {
    int host_level = level;
    int host_optname = optname;

    switch (level) {
        case SOCKETPOOL_SOL_SOCKET:
            host_level = SOL_SOCKET;
            break;
        case SOCKETPOOL_IPPROTO_IP:
            host_level = IPPROTO_IP;
            break;
        case SOCKETPOOL_IPPROTO_TCP:
            host_level = IPPROTO_TCP;
            break;
        case SOCKETPOOL_IPPROTO_UDP:
            host_level = IPPROTO_UDP;
            break;
        #if CIRCUITPY_SOCKETPOOL_IPV6
        case SOCKETPOOL_IPPROTO_IPV6:
            host_level = IPPROTO_IPV6;
            break;
        #endif
    }

    if (host_level == SOL_SOCKET) {
        switch (optname) {
            case SOCKETPOOL_SO_REUSEADDR:
                host_optname = SO_REUSEADDR;
                break;
        }
    } else if (host_level == IPPROTO_TCP) {
        switch (optname) {
            case SOCKETPOOL_TCP_NODELAY:
                host_optname = TCP_NODELAY;
                break;
        }
    }

    int err = setsockopt(self->num, host_level, host_optname, HOST_SOCK_CBUF(value), optlen);
    if (err != 0) {
        return -host_sock_errno();
    }
    return 0;
}

bool common_hal_socketpool_readable(socketpool_socket_obj_t *self) {
    struct pollfd fd = {
        .fd = self->num,
        .events = POLLIN,
    };

    int num_triggered = host_poll(&fd, 1, 0);
    return num_triggered > 0;
}

bool common_hal_socketpool_writable(socketpool_socket_obj_t *self) {
    struct pollfd fd = {
        .fd = self->num,
        .events = POLLOUT,
    };

    int num_triggered = host_poll(&fd, 1, 0);
    return num_triggered > 0;
}

void socketpool_socket_move(socketpool_socket_obj_t *self, socketpool_socket_obj_t *sock) {
    *sock = *self;
    self->connected = false;
    self->num = -1;
}

void socketpool_socket_reset(socketpool_socket_obj_t *self) {
    if (self->base.type == &socketpool_socket_type) {
        return;
    }
    self->base.type = &socketpool_socket_type;
    self->connected = false;
    self->num = -1;
}
