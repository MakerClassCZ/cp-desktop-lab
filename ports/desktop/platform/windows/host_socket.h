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

#include <errno.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#define SHUT_RDWR SD_BOTH
#define MSG_NOSIGNAL 0
#define MSG_DONTWAIT 0          // every socket is non-blocking already
#define HOST_SOCK_BUF(p) ((char *)(p))
#define HOST_SOCK_CBUF(p) ((const char *)(p))

// A Winsock error as the errno the BSD call would have set.
static inline int host_sock_errno_of(int err) {
    switch (err) {
        case 0:
            return 0;
        case WSAEWOULDBLOCK:
            return EAGAIN;
        case WSAEINPROGRESS:
        case WSAEALREADY:
            return EINPROGRESS;
        case WSAEINTR:
            return EINTR;
        case WSAEBADF:
        case WSAENOTSOCK:
            return EBADF;
        case WSAEACCES:
            return EACCES;
        case WSAEINVAL:
            return EINVAL;
        case WSAEMFILE:
            return EMFILE;
        case WSAEADDRINUSE:
            return EADDRINUSE;
        case WSAEADDRNOTAVAIL:
            return EADDRNOTAVAIL;
        case WSAENETDOWN:
        case WSAENETUNREACH:
            return ENETUNREACH;
        case WSAECONNABORTED:
            return ECONNABORTED;
        case WSAECONNRESET:
            return ECONNRESET;
        case WSAENOBUFS:
            return ENOBUFS;
        case WSAEISCONN:
            return EISCONN;
        case WSAENOTCONN:
        case WSAESHUTDOWN:
            return ENOTCONN;
        case WSAETIMEDOUT:
            return ETIMEDOUT;
        case WSAECONNREFUSED:
            return ECONNREFUSED;
        case WSAEHOSTUNREACH:
            return EHOSTUNREACH;
        case WSAEAFNOSUPPORT:
            return EAFNOSUPPORT;
        case WSAEPROTONOSUPPORT:
            return EPROTONOSUPPORT;
        default:
            return EIO;
    }
}

static inline int host_sock_errno(void) {
    return host_sock_errno_of(WSAGetLastError());
}

static inline int host_poll(struct pollfd *fds, unsigned count, int timeout_ms) {
    return WSAPoll(fds, count, timeout_ms);
}

static inline int host_sock_close(int fd) {
    shutdown(fd, SD_BOTH);
    return closesocket(fd);
}

static inline int host_sock_set_nonblocking(int fd) {
    u_long one = 1;
    return ioctlsocket(fd, FIONBIO, &one);
}
