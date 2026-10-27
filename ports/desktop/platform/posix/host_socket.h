// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The host's sockets with one face: BSD sockets on POSIX, Winsock on Windows. The socket code
// is written to the BSD API and uses these for the places where Winsock differs: closing,
// non-blocking mode, errors (WSAGetLastError instead of errno, with its own numbers), poll
// (WSAPoll) and the char * buffers.

#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
// macOS has no such flag: each socket is made SO_NOSIGPIPE instead (host_sock_nosigpipe).
#define MSG_NOSIGNAL 0
#endif

// A send to a peer that has gone fails with EPIPE instead of ending the process with SIGPIPE,
// which on macOS comes even with the signal blocked.
static inline void host_sock_nosigpipe(int fd) {
    #ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    #else
    (void)fd;
    #endif
}

#define HOST_SOCK_BUF(p) ((void *)(p))
#define HOST_SOCK_CBUF(p) ((const void *)(p))

static inline int host_sock_errno_of(int err) {
    return err;
}

static inline int host_sock_errno(void) {
    return errno;
}

static inline int host_poll(struct pollfd *fds, unsigned count, int timeout_ms) {
    return poll(fds, count, timeout_ms);
}

static inline int host_sock_close(int fd) {
    shutdown(fd, SHUT_RDWR);
    return close(fd);
}

static inline int host_sock_set_nonblocking(int fd) {
    return fcntl(fd, F_SETFL, O_NONBLOCK);
}
