// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The process on POSIX: its environment, the fault signals, re-executing itself as a reset, the
// termination signals on a thread of their own, the stack limit. /proc/self/exe is Linux's,
// _NSGetExecutablePath macOS's.

#include <execinfo.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "desktop_port.h"
#include "platform.h"
#include "sdl_host.h"

void platform_set_env(const char *name, const char *value) {
    if (value != NULL) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}

static void (*_restart)(void);

static void _on_fault(int sig) {
    static const char msg[] = "\r\ncircuitpython: hard fault, backtrace:\r\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    void *frames[32];
    backtrace_symbols_fd(frames, backtrace(frames, 32), STDERR_FILENO);
    _restart();
}

void platform_catch_faults(void (*restart)(void)) {
    _restart = restart;
    struct sigaction fault = {0};
    fault.sa_handler = _on_fault;
    fault.sa_flags = SA_RESETHAND | SA_NODEFER;
    sigaction(SIGSEGV, &fault, NULL);
    sigaction(SIGBUS, &fault, NULL);
    sigaction(SIGILL, &fault, NULL);
    sigaction(SIGFPE, &fault, NULL);
    sigset_t faults;
    sigemptyset(&faults);
    sigaddset(&faults, SIGSEGV);
    sigaddset(&faults, SIGBUS);
    sigaddset(&faults, SIGILL);
    sigaddset(&faults, SIGFPE);
    pthread_sigmask(SIG_UNBLOCK, &faults, NULL);
}

// The program's own path, NUL terminated in path; false when it is not known.
static bool _exe_path(char *path, size_t len) {
    #ifdef __APPLE__
    // The path the program was started by, which may hold symbolic links and "..".
    char started[PATH_MAX];
    uint32_t size = sizeof(started);
    if (_NSGetExecutablePath(started, &size) != 0 || len < PATH_MAX) {
        return false;
    }
    return realpath(started, path) != NULL;
    #else
    ssize_t n = readlink("/proc/self/exe", path, len - 1);
    if (n <= 0 || (size_t)n >= len) {
        return false;
    }
    path[n] = '\0';
    return true;
    #endif
}

void platform_restart(int generation) {
    (void)generation;
    char path[PATH_MAX];
    if (_exe_path(path, sizeof(path))) {
        execv(path, desktop_options.argv);
    }
    perror("circuitpython: reset");
}

// Termination signals end up here, so the VM thread never sees them.
static void *_signal_main(void *arg) {
    (void)arg;
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGHUP);
    sigaddset(&stop, SIGQUIT);
    sigaddset(&stop, SIGUSR1);
    for (;;) {
        int sig = 0;
        if (sigwait(&stop, &sig) != 0) {
            continue;
        }
        if (sig == SIGUSR1) {
            // kill -USR1 saves the next frame as a screenshot
            desktop_sdl_request_screenshot();
            continue;
        }
        desktop_power_off(128 + sig);
    }
}

bool platform_start(char *drive_path) {
    (void)drive_path;
    // Every thread starts with all signals blocked: platform_catch_faults() unblocks the fault
    // signals on the VM's thread, and the signal thread waits for the termination ones.
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    pthread_t signal_thread;
    if (pthread_create(&signal_thread, NULL, _signal_main, NULL) != 0) {
        perror("circuitpython: signal thread");
        return false;
    }
    return true;
}

size_t platform_stack_limit(void) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_STACK, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY) {
        return limit.rlim_cur;
    }
    return 0;
}

void platform_exe_dir(char *dir, size_t len) {
    if (_exe_path(dir, len)) {
        char *slash = strrchr(dir, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
    } else {
        snprintf(dir, len, ".");
    }
}

bool platform_is_absolute(const char *path) {
    return path[0] == '/';
}

static pthread_t _vm_thread;

void platform_mark_vm_thread(void) {
    _vm_thread = pthread_self();
}

bool desktop_on_vm_thread(void) {
    return pthread_equal(pthread_self(), _vm_thread);
}
