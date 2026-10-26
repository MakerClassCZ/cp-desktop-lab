// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The process on Windows: its environment, the exception filter, a reset as the same command
// line run again and waited for, the console's code page and control events, Winsock.

#include <winsock2.h>
#include <windows.h>
#include <ctype.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/runtime.h"

#include "desktop_port.h"
#include "platform.h"

// A Windows reset leaves the parent waiting, so a program that keeps resetting is cut off here.
#define RESTARTS_MAX 16

void platform_set_env(const char *name, const char *value) {
    SetEnvironmentVariableA(name, value);
    _putenv_s(name, value != NULL ? value : "");
}

static void (*_restart)(void);

// The image base the linker chose, read from the program's file: the loader rewrites the copy in
// memory to where it put the image, so that one cannot turn an address back into addr2line's.
static uintptr_t _linked_base;

static void _read_linked_base(void) {
    char path[MAX_PATH];
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS nt;
    FILE *f = GetModuleFileNameA(NULL, path, sizeof(path)) > 0 ? fopen(path, "rb") : NULL;
    if (f != NULL && fread(&dos, sizeof(dos), 1, f) == 1 && fseek(f, dos.e_lfanew, SEEK_SET) == 0
        && fread(&nt, sizeof(nt), 1, f) == 1) {
        _linked_base = (uintptr_t)nt.OptionalHeader.ImageBase;
    }
    if (f != NULL) {
        fclose(f);
    }
}

// An address in the program as addr2line takes it; 0 for one outside it (a DLL).
static unsigned long long _linked(uintptr_t address) {
    HMODULE module = NULL;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)address, &module);
    return (uintptr_t)module == base ? (unsigned long long)(_linked_base + (address - base)) : 0;
}

static LONG WINAPI _on_exception(EXCEPTION_POINTERS *info) {
    EXCEPTION_RECORD *record = info->ExceptionRecord;
    fprintf(stderr, "\r\ncircuitpython: hard fault, exception 0x%lx; backtrace for addr2line -f -e circuitpython.exe:\r\n",
        (unsigned long)record->ExceptionCode);
    // The stack, unwound frame by frame from the faulting context.
    CONTEXT context = *info->ContextRecord;
    for (int i = 0; i < 24 && context.Rip != 0; i++) {
        unsigned long long linked = _linked((uintptr_t)context.Rip);
        if (linked != 0) {
            fprintf(stderr, "  0x%llx\r\n", linked);
        } else {
            fprintf(stderr, "  0x%llx (outside the program)\r\n", (unsigned long long)context.Rip);
        }
        DWORD64 image_base;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, NULL);
        if (function == NULL) {
            // A leaf function: the return address is on top of the stack.
            context.Rip = *(DWORD64 *)context.Rsp;
            context.Rsp += 8;
        } else {
            void *handler_data;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher, NULL);
        }
    }
    _restart();
    return EXCEPTION_EXECUTE_HANDLER;
}

void platform_catch_faults(void (*restart)(void)) {
    _restart = restart;
    _read_linked_base();
    SetUnhandledExceptionFilter(_on_exception);
}

void platform_restart(int generation) {
    // Windows cannot replace a running process: run the same command line again, wait for it and
    // end with its status, so the console session is one program from start to finish. Each
    // generation keeps its parent waiting, hence the limit.
    if (generation > RESTARTS_MAX) {
        fprintf(stderr, "circuitpython: reset: too many restarts, ending\r\n");
        _exit(1);
    }
    char *command = strdup(GetCommandLineA());
    STARTUPINFOA startup = { .cb = sizeof(startup) };
    PROCESS_INFORMATION process;
    if (command != NULL && CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        DWORD status = 1;
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &status);
        _exit(status);
    }
    fprintf(stderr, "circuitpython: reset: error %lu\r\n", (unsigned long)GetLastError());
}

// Closing the console window, Ctrl-Break and a logoff end the program; Ctrl-C only arrives here
// when stdin is not the console (the raw console delivers it as a byte).
static BOOL WINAPI _ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT) {
        mp_sched_keyboard_interrupt();
        desktop_wake_vm();
        return TRUE;
    }
    desktop_power_off(128 + 2);
}

bool platform_start(char *drive_path) {
    // VfsPosix joins paths with '/', which Windows takes as well.
    for (char *c = drive_path; *c != '\0'; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    // Files are bytes, the console speaks UTF-8, and the network is up.
    _set_fmode(_O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleCtrlHandler(_ctrl_handler, TRUE);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    return true;
}

size_t platform_stack_limit(void) {
    return 0;                           // the main thread's stack is set when linking
}

void platform_exe_dir(char *dir, size_t len) {
    DWORD n = GetModuleFileNameA(NULL, dir, (DWORD)len);
    if (n > 0 && (size_t)n < len) {
        dir[n] = '\0';
        char *slash = strrchr(dir, '/');
        char *backslash = strrchr(dir, '\\');
        if (backslash != NULL && (slash == NULL || backslash > slash)) {
            slash = backslash;
        }
        if (slash != NULL) {
            *slash = '\0';
        }
    } else {
        snprintf(dir, len, ".");
    }
}

bool platform_is_absolute(const char *path) {
    return path[0] == '/' || path[0] == '\\' || (isalpha((unsigned char)path[0]) && path[1] == ':');
}

static DWORD _vm_thread;

void platform_mark_vm_thread(void) {
    _vm_thread = GetCurrentThreadId();
}

bool desktop_on_vm_thread(void) {
    return GetCurrentThreadId() == _vm_thread;
}
