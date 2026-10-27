// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

// The VM runs on its own thread with a stack of this size (desktop_main.c).
#define CIRCUITPY_DEFAULT_STACK_SIZE            (1024 * 1024)

// The object representation packs floats into 32-bit words; on a 64-bit host use the plain one.
#if UINTPTR_MAX > 0xffffffffu
#define MICROPY_OBJ_REPR                        (MICROPY_OBJ_REPR_A)
#endif

// The processor the binary was built for.
#if defined(__x86_64__)
#define MICROPY_HW_MCU_NAME                     "x86_64"
#elif defined(__i386__)
#define MICROPY_HW_MCU_NAME                     "i386"
#elif defined(__aarch64__)
#define MICROPY_HW_MCU_NAME                     "aarch64"
#elif defined(__arm__)
#define MICROPY_HW_MCU_NAME                     "arm"
#elif defined(__riscv)
#define MICROPY_HW_MCU_NAME                     "riscv"
#else
#define MICROPY_HW_MCU_NAME                     "unknown"
#endif

// The Python call stack: a board's 2 KB would end a traced program early, since the debugger
// adds frames of its own above every call of the user's.
#define CIRCUITPY_PYSTACK_SIZE                  (256 * 1024)

// sys.settrace, the hook a debugger steps and inspects with; a host can afford its cost.
#define MICROPY_PY_SYS_SETTRACE                 (1)
// sys.platform as CPython names the host; libraries read it to tell boards apart.
#ifdef _WIN32
#define MICROPY_PY_SYS_PLATFORM                 "win32"
#else
#define MICROPY_PY_SYS_PLATFORM                 "linux"
#endif
// ... unless a board file names the real board's (desktop_board.c): a library may pick by it.
#define MICROPY_PY_SYS_PLATFORM_OBJ             desktop_sys_platform
// os.uname().machine likewise, which adafruit_pybadger and the like pick by.
#define CIRCUITPY_OS_UNAME_MACHINE_OBJ          desktop_os_machine
// sys.exc_info(): the trace hook's "exception" event comes without the exception, this has it.
#define MICROPY_PY_SYS_EXC_INFO                 (1)

// The register helper's x86-64 assembly is for the System V ABI, and its arm64 register variables
// are GCC's; Windows and macOS (clang) use setjmp instead.
#if defined(_WIN32) || defined(__APPLE__)
#define MICROPY_GCREGS_SETJMP                   (1)
#endif

// The board is chosen at run time (--board): the board module's dictionary points at the chosen
// table (desktop_board.c).
#define CIRCUITPY_MUTABLE_BOARD                 (1)

// An FT232H has pull-ups only and an MCP2221 none, and neither drives open drain, so digitalio
// can refuse a pull or a drive mode.
#define CIRCUITPY_DIGITALIO_HAVE_INVALID_PULL   (1)
#define CIRCUITPY_DIGITALIO_HAVE_INVALID_DRIVE_MODE (1)

// board.SPI() and board.I2C() on the chosen board's bus pins: busio translates these placeholders.
#define DEFAULT_SPI_BUS_SCK                     (&pin_BUS_SCK)
#define DEFAULT_SPI_BUS_MOSI                    (&pin_BUS_MOSI)
#define DEFAULT_SPI_BUS_MISO                    (&pin_BUS_MISO)
#define DEFAULT_I2C_BUS_SCL                     (&pin_BUS_SCL)
#define DEFAULT_I2C_BUS_SDA                     (&pin_BUS_SDA)
// board.UART() on the host serial port (--uart, or the MCP2221's own).
#define DEFAULT_UART_BUS_TX                     (&pin_UART_TX)
#define DEFAULT_UART_BUS_RX                     (&pin_UART_RX)

// CIRCUITPY can be a host directory (VfsPosix) as well as a FAT image (--drive).
#ifndef CIRCUITPY_FILESYSTEM_POSIX
#define CIRCUITPY_FILESYSTEM_POSIX              (1)
#endif
// VfsPosix's statvfs, for os.statvfs() on a directory CIRCUITPY; on Windows from platform/windows.
#define MICROPY_PY_OS_STATVFS                   (1)

// board.DISPLAY and up to three the program puts on the buses (a TFT, an OLED, an RGB matrix).
#define CIRCUITPY_DISPLAY_LIMIT                 (4)

// boot_out.txt is written at once: a computer's power does not fail as a battery's may.
#define CIRCUITPY_BOOT_OUTPUT_DELAY_MS          (0)

// Also includes mpconfigboard.h
#include "py/circuitpy_mpconfig.h"

// select.poll waits out its timeout here (interrupts.c): nothing, unless fast time skips it.
void desktop_fast_wait_ms(int timeout_ms);
#define MICROPY_INTERNAL_WFE(TIMEOUT_MS) desktop_fast_wait_ms(TIMEOUT_MS)

// The board file's names become qstrs in each VM (board_file.c), so Tab in the REPL offers them.
void board_file_intern_names(void);
#define MICROPY_PORT_INIT_FUNC board_file_intern_names()
