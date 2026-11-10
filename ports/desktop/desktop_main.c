// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The process entry: parse the options, run the VM on the main thread (SDL wants its window
// and events there on some systems) with a stack of known bounds, and turn termination signals
// into a clean exit that restores the terminal.

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif

#include "py/runtime.h"

#include "audio_capture.h"
#include "playbook.h"
#include "ble_host.h"
#include "bus_trace.h"
#include "control_api.h"
#include "desktop_pins.h"
#include "desktop_port.h"
#include "parts.h"
#include "platform.h"
#include "sdl_host.h"

int circuitpython_main(void);

#define MIB (1024 * 1024)
#define VM_STACK_SIZE (2 * MIB)
#define STACK_MARGIN (256 * 1024)       // left to the kernel's guard gap and to signal frames
#define PAGE (4096)

desktop_options_t desktop_options = {
    .drive_path = "CIRCUITPY",
    .drive_size = 16 * MIB,
    .ram_size = 16 * MIB,
    .board_name = "virtual",
    .gpiochip = "/dev/gpiochip0",
    .i2c_bus = 1,
    .spidev = "/dev/spidev0.0",
    .api_port = -1,
};

uint8_t *desktop_stack_bottom;
uint8_t *desktop_stack_top;

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [--board NAME] [--drive PATH] [--drive-size MB] [--nvm PATH] [--ram MB]\n"
        "  --board NAME      virtual (default), ft232h or mcp2221 (real pins through that chip on USB),\n"
        "                    gpiochip (this computer's own GPIO, I2C and SPI, as on a Raspberry Pi)\n"
        "  --list-boards     the boards, and whether each one's adapter is plugged in\n"
        "  --board-file FILE more pin names ([pins]), NeoPixel layouts and parts, TOML; more than\n"
        "                    one are read in order, a later one's part replacing an earlier one's\n"
        "  --neopixel N=L    the NeoPixels on pin N laid out as L: 10, 8x8, \"16x16 serpentine GRBW\"\n"
        "  --trace-bus PATH  write every I2C, SPI and UART transfer to PATH (- = stderr)\n"
        "  --uart PORT       busio.UART on board.TX/board.RX uses this serial port (COM5, /dev/ttyUSB0)\n"
        "  --ble PORT        _bleio's Bluetooth controller on this serial port (a Pico 2 W bridge,\n"
        "                    Zephyr's hci_uart); off for none; default the virtual one\n"
        "  --ble-log PATH    write every Bluetooth HCI packet to PATH as btsnoop (Wireshark)\n"
        "  --ble-bonds PATH  the Bluetooth bonds' file (default the drive's path + .bonds, beside it)\n"
        "  --ble-workflow M  the BLE workflow (files and the REPL over Bluetooth, as on a CLUE): on,\n"
        "                    discover (bonds erased, advertised as CIRCUITPY, once) or off (default)\n"
        "  --code FILE       run FILE on the drive (games/snake.py) in place of code.py, every run\n"
        "  --gpiochip PATH   --board gpiochip: the GPIO chip (default /dev/gpiochip0)\n"
        "  --i2c-bus N       --board gpiochip: /dev/i2c-N (default 1)\n"
        "  --spi PATH        --board gpiochip: the spidev device (default /dev/spidev0.0)\n"
        "  --drive PATH      CIRCUITPY: a directory (default ./CIRCUITPY), or a FAT image file;\n"
        "                    a missing PATH is created, as an image if it ends in .img\n"
        "  --drive-size MB   size of a newly created image (default 16)\n"
        "  --nvm PATH        microcontroller.nvm's file (default the drive's path + .nvm, beside it)\n"
        "  --ram MB          heap for CircuitPython (default 16)\n"
        "  --heap KB         the VM's heap grows to KB at most (default: as far as --ram allows)\n"
        "  --profile NAME    the VM's heap as large as a board's: rp2040, samd51, esp32s3, rp2350\n"
        "  --debug [PORT]    wait for a debugger (VS Code) on 127.0.0.1:PORT before code.py (default 5678)\n"
        "  --api [PORT]      the control API on 127.0.0.1:PORT (default 8765, 0 = any free port)\n"
        "  --api-host ADDR   listen on ADDR instead; needs CIRCUITPY_API_TOKEN\n"
        "  --fast-time       sleeps return at once and move the program's clock on (virtual board)\n"
        "  --audio-capture   record what the audio outputs play, for the API's /api/audio\n"
        "  --playbook FILE   play FILE's panel lines, waits and checks at each code.py run\n"
        "Ctrl-\\ on the console powers off.\n", prog);
}

#define DEBUG_PORT 5678
#define API_PORT 8765

// An option's port: the next argument when it is a number, else the default.
static int optional_port(int argc, char **argv, int *i, int fallback) {
    if (*i + 1 < argc && isdigit((unsigned char)argv[*i + 1][0])) {
        return atoi(argv[++*i]);
    }
    return fallback;
}

// A file's path: given (relative to the start directory), else the drive's path and ext beside
// it. False when it does not fit.
static bool beside_drive(char *out, size_t cap, const char *given, const char *ext, const char *drive_path,
    const char *start_dir) {
    int len;
    if (given == NULL) {
        int n = (int)strlen(drive_path);
        while (n > 1 && (drive_path[n - 1] == '/' || drive_path[n - 1] == '\\')) {
            n--;
        }
        len = snprintf(out, cap, "%.*s%s", n, drive_path, ext);
    } else {
        len = platform_is_absolute(given)
            ? snprintf(out, cap, "%s", given)
            : snprintf(out, cap, "%s/%s", start_dir, given);
    }
    return len >= 0 && (size_t)len < cap;
}

// Real hardware (another board, a serial port, the computer's microphone) needs real time.
static bool fast_time_allowed(void) {
    if (desktop_pin_backend != &desktop_virtual_backend || desktop_options.uart_path != NULL
        || ble_host_port() != NULL) {
        return false;
    }
    for (int i = 0; i < parts_count(); i++) {
        part_t *part = parts_at(i);
        if (part->kind == PART_MIC && part->source == MIC_HOST) {
            return false;
        }
    }
    return true;
}

// The VM's heap limit at which as many 4 KB bytearrays fit in this (64-bit) program as at a
// board's REPL with nothing else running: 37, 30, 526 and 2014 of them (measured 2026-11-10 on a
// PicoPad, a PyBadge (SAMD51J19), a Feather ESP32-S3 TFT (2 MB PSRAM) and a Fruit Jam (RP2350B,
// 8 MB PSRAM), CircuitPython 10 and 11 alpha).
static const struct {
    const char *name;
    size_t heap;
} _profiles[] = {
    { "rp2040", 169 * 1024 },
    { "samd51", 141 * 1024 },
    { "esp32s3", 2170 * 1024 },
    { "rp2350", 8266 * 1024 },
};

static size_t parse_mib(const char *s, const char *prog) {
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || v == 0 || v > 4096) {
        usage(prog);
        exit(2);
    }
    return (size_t)v * MIB;
}

// Map the stack below this frame now, page by page from the top: CircuitPython writes a canary at
// the stack limit at once, and an older kernel faults on an access that far below the stack
// pointer instead of growing the stack. Returns the lowest address mapped.
__attribute__((noinline)) static uintptr_t grow_stack(size_t bytes) {
    volatile uint8_t *probe = alloca(bytes);
    for (size_t offset = PAGE; offset <= bytes; offset += PAGE) {
        probe[bytes - offset] = 0;
    }
    return (uintptr_t)probe;
}

int main(int argc, char **argv) {
    desktop_options.argv = argv;
    platform_mark_vm_thread();
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list-boards") == 0) {
            desktop_board_list();
            return 0;
        } else if (strcmp(argv[i], "--board") == 0 && i + 1 < argc) {
            desktop_options.board_name = argv[++i];
        } else if (strcmp(argv[i], "--trace-bus") == 0 && i + 1 < argc) {
            desktop_options.trace_bus = argv[++i];
        } else if (strcmp(argv[i], "--uart") == 0 && i + 1 < argc) {
            desktop_options.uart_path = argv[++i];
        } else if (strcmp(argv[i], "--ble") == 0 && i + 1 < argc) {
            desktop_options.ble = argv[++i];
        } else if (strcmp(argv[i], "--ble-log") == 0 && i + 1 < argc) {
            desktop_options.ble_log = argv[++i];
        } else if (strcmp(argv[i], "--ble-bonds") == 0 && i + 1 < argc) {
            desktop_options.ble_bonds_path = argv[++i];
        } else if (strcmp(argv[i], "--ble-workflow") == 0 && i + 1 < argc) {
            desktop_options.ble_workflow = argv[++i];
        } else if (strcmp(argv[i], "--code") == 0 && i + 1 < argc) {
            desktop_options.code = argv[++i];
        } else if (strcmp(argv[i], "--board-file") == 0 && i + 1 < argc) {
            if (desktop_options.board_file_count == 8) {
                fprintf(stderr, "circuitpython: more than 8 board files\n");
                return 2;
            }
            desktop_options.board_files[desktop_options.board_file_count++] = argv[++i];
        } else if (strcmp(argv[i], "--neopixel") == 0 && i + 1 < argc && desktop_options.neopixel_count < 8) {
            desktop_options.neopixel[desktop_options.neopixel_count++] = argv[++i];
        } else if (strcmp(argv[i], "--gpiochip") == 0 && i + 1 < argc) {
            desktop_options.gpiochip = argv[++i];
        } else if (strcmp(argv[i], "--i2c-bus") == 0 && i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) {
            desktop_options.i2c_bus = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--spi") == 0 && i + 1 < argc) {
            desktop_options.spidev = argv[++i];
        } else if (strcmp(argv[i], "--drive") == 0 && i + 1 < argc) {
            desktop_options.drive_path = argv[++i];
        } else if (strcmp(argv[i], "--nvm") == 0 && i + 1 < argc) {
            desktop_options.nvm_path = argv[++i];
        } else if (strcmp(argv[i], "--drive-size") == 0 && i + 1 < argc) {
            desktop_options.drive_size = parse_mib(argv[++i], argv[0]);
        } else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
            desktop_options.ram_size = parse_mib(argv[++i], argv[0]);
        } else if (strcmp(argv[i], "--debug") == 0) {
            desktop_options.debug_port = optional_port(argc, argv, &i, DEBUG_PORT);
        } else if (strcmp(argv[i], "--api") == 0) {
            desktop_options.api_port = optional_port(argc, argv, &i, API_PORT);
        } else if (strcmp(argv[i], "--api-host") == 0 && i + 1 < argc) {
            desktop_options.api_host = argv[++i];
        } else if (strcmp(argv[i], "--heap") == 0 && i + 1 < argc) {
            char *end;
            unsigned long kb = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || kb == 0) {
                usage(argv[0]);
                return 2;
            }
            desktop_options.heap_limit = (size_t)kb * 1024;
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            const char *name = argv[++i];
            for (size_t k = 0; k < MP_ARRAY_SIZE(_profiles); k++) {
                if (strcmp(name, _profiles[k].name) == 0) {
                    desktop_options.heap_limit = _profiles[k].heap;
                    desktop_options.profile = _profiles[k].name;
                }
            }
            if (desktop_options.profile == NULL) {
                fprintf(stderr, "circuitpython: no profile %s; known: rp2040, samd51, esp32s3, rp2350\n", name);
                return 2;
            }
            if (sizeof(void *) == 8) {
                fprintf(stderr, "circuitpython: profile %s on a 64-bit program: buffers fit as on the board, "
                    "small objects (lists, dicts, instances) take up to twice the room\n", name);
            }
        } else if (strcmp(argv[i], "--fast-time") == 0) {
            desktop_options.fast_time = true;
        } else if (strcmp(argv[i], "--audio-capture") == 0) {
            desktop_options.audio_capture = true;
        } else if (strcmp(argv[i], "--playbook") == 0 && i + 1 < argc) {
            if (!playbook_load(argv[++i])) {
                return 2;
            }
        } else {
            usage(argv[0]);
            return strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }

    if (!desktop_board_select(desktop_options.board_name)) {
        fprintf(stderr, "circuitpython: unknown board %s; known: %s\n", desktop_options.board_name, desktop_board_names());
        return 2;
    }
    if (!board_file_apply()) {
        return 2;
    }
    if (desktop_options.fast_time && !fast_time_allowed()) {
        fprintf(stderr, "circuitpython: --fast-time runs only on the virtual board without --uart, --ble PORT or a microphone: real hardware needs real time\n");
        return 2;
    }

    if (desktop_options.audio_capture) {
        audio_capture_start();
    }

    // VfsPosix moves the working directory into CIRCUITPY, so keep paths absolute.
    static char start_dir[PATH_MAX];
    static char drive_path[PATH_MAX];
    if (getcwd(start_dir, sizeof(start_dir)) == NULL) {
        perror("circuitpython: working directory");
        return 1;
    }
    desktop_options.start_dir = start_dir;

    // The program's own directory: the debug adapter is a Python file kept beside it.
    static char exe_dir[PATH_MAX];
    platform_exe_dir(exe_dir, sizeof(exe_dir));
    desktop_options.exe_dir = exe_dir;

    // Opened before the drive's directory becomes the working directory.
    if (desktop_options.trace_bus != NULL && !bus_trace_open(desktop_options.trace_bus)) {
        return 2;
    }

    if (!ble_host_init()) {
        return 2;
    }

    // The pins' transport (a USB device for a real board) opens before anything else runs.
    if (!desktop_pin_backend->open()) {
        return 2;
    }

    int len = platform_is_absolute(desktop_options.drive_path)
        ? snprintf(drive_path, sizeof(drive_path), "%s", desktop_options.drive_path)
        : snprintf(drive_path, sizeof(drive_path), "%s/%s", start_dir, desktop_options.drive_path);
    if (len < 0 || (size_t)len >= sizeof(drive_path)) {
        fprintf(stderr, "circuitpython: drive path too long\n");
        return 2;
    }
    desktop_options.drive_path = drive_path;
    // microcontroller.nvm and the Bluetooth bonds: beside the drive, never on it (CIRCUITPY/
    // makes CIRCUITPY.nvm and CIRCUITPY.bonds too).
    static char nvm_path[PATH_MAX], bonds_path[PATH_MAX];
    if (!beside_drive(nvm_path, sizeof(nvm_path), desktop_options.nvm_path, ".nvm", drive_path, start_dir) ||
        !beside_drive(bonds_path, sizeof(bonds_path), desktop_options.ble_bonds_path, ".bonds", drive_path, start_dir)) {
        fprintf(stderr, "circuitpython: nvm or bonds path too long\n");
        return 2;
    }
    desktop_options.nvm_path = nvm_path;
    desktop_options.ble_bonds_path = bonds_path;
    // A drive that is not there is made, empty but for code.py: said, as a typo in the path makes one too.
    struct stat st;
    if (stat(drive_path, &st) != 0) {
        fprintf(stderr, "circuitpython: no drive at %s: a new one is made\n", drive_path);
    }

    // The VM gets VM_STACK_SIZE of the main stack, less if the stack limit is lower.
    size_t stack_size = VM_STACK_SIZE;
    if (!platform_start(drive_path)) {
        return 1;
    }
    size_t limit = platform_stack_limit();
    if (limit != 0 && limit < stack_size + 2 * STACK_MARGIN) {
        stack_size = limit > 3 * STACK_MARGIN ? limit - 2 * STACK_MARGIN : STACK_MARGIN;
    }
    if (desktop_options.api_host != NULL && desktop_options.api_port < 0) {
        desktop_options.api_port = API_PORT;
    }
    if (!control_api_start()) {
        return 2;
    }
    desktop_stack_top = (uint8_t *)__builtin_frame_address(0);
    desktop_stack_bottom = (uint8_t *)(grow_stack(stack_size) + PAGE);
    circuitpython_main();
    desktop_power_off(0);
}
