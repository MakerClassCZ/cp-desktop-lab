// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "supervisor/port.h"

#include "after_run.h"
#include "airlift.h"
#include "ble_host.h"
#include "control_api.h"
#include "epaper_virtual.h"
#include "playbook.h"

#if CIRCUITPY_SDL
#include "sdl_host.h"
#endif
#if CIRCUITPY_WATCHDOG
#include "common-hal/watchdog/WatchDogTimer.h"
#endif
#if CIRCUITPY_AUDIOIO
#include "common-hal/audioio/AudioOut.h"
#endif

// Once per tick (1024 Hz while the display or sound keeps the tick on): window events, frames and
// the audio queue. port_background_task() runs at every VM loop hook, far too often for these.
void port_background_tick(void) {
    #if CIRCUITPY_SDL
    desktop_sdl_task();
    #endif
    #if CIRCUITPY_AUDIOIO
    audioout_background();
    #endif
}

void port_background_task(void) {
    after_run_task();
    control_api_vm_task();
    airlift_vm_task();
    epaper_virtual_task();
    playbook_task();
    ble_host_vm_task();
    #if CIRCUITPY_WATCHDOG
    desktop_watchdog_check();
    #endif
}

void port_start_background_tick(void) {
}

void port_finish_background_tick(void) {
}
