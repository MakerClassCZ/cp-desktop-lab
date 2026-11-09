// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include <SDL.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "py/mpconfig.h"
#include "supervisor/shared/tick.h"

#include "gamepad.h"
#include "desktop_port.h"
#include "midi_in.h"
#include "panel.h"
#include "parts.h"
#include "sdl_host.h"
#include "touch.h"

#define FRAME_MS 16
// Pixels that changed this recently are still changing.
#define CHANGING_MS 250

static SDL_Window *_window;
static SDL_Renderer *_renderer;
static SDL_Texture *_texture;
static uint16_t *_pixels;
static int _width;
static int _height;
static int _scale = 2;
static bool _fullscreen;
static uint64_t _last_frame_ms;
// The window shows something other than the pixels: drawn again at the next frame.
static bool _stale;
// The pixels last presented: a program that writes the buffer without a refresh (picogame on a
// framebuffer) is presented when they differ.
static uint16_t *_shown;
static int _screenshot_requested;
static int _screenshot_count;

// Game controllers, which is how SDL reports a handheld's own buttons.
static int _pad_button(uint8_t button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return DESKTOP_BUTTON_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return DESKTOP_BUTTON_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return DESKTOP_BUTTON_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return DESKTOP_BUTTON_RIGHT;
        case SDL_CONTROLLER_BUTTON_A:
            return DESKTOP_BUTTON_A;
        case SDL_CONTROLLER_BUTTON_B:
            return DESKTOP_BUTTON_B;
        case SDL_CONTROLLER_BUTTON_X:
            return DESKTOP_BUTTON_X;
        case SDL_CONTROLLER_BUTTON_Y:
            return DESKTOP_BUTTON_Y;
        case SDL_CONTROLLER_BUTTON_START:
            return DESKTOP_BUTTON_START;
        case SDL_CONTROLLER_BUTTON_BACK:
            return DESKTOP_BUTTON_SELECT;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return DESKTOP_BUTTON_L;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return DESKTOP_BUTTON_R;
        case SDL_CONTROLLER_BUTTON_GUIDE:
            return DESKTOP_BUTTON_MENU;
        default:
            return -1;
    }
}

static void _save_screenshot(void) {
    char name[PATH_MAX];
    const char *path = getenv("CIRCUITPY_SCREENSHOT");
    if (path == NULL) {
        // In the starting directory: CIRCUITPY may be the working directory now.
        snprintf(name, sizeof(name), "%s/screenshot-%03d.bmp", desktop_options.start_dir, _screenshot_count++);
        path = name;
    }
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(_pixels, _width, _height, 16,
        _width * 2, SDL_PIXELFORMAT_RGB565);
    if (surface != NULL && SDL_SaveBMP(surface, path) == 0) {
        fprintf(stderr, "circuitpython: screenshot %s\r\n", path);
    }
    SDL_FreeSurface(surface);
}

// CIRCUITPY_SDL_KEYS names a file or FIFO of the panel's lines (panel.h: "down Z", "set A0 100"
// ...) and "sleep MS", so a test can play a program headless.
static void *_key_script(void *arg) {
    FILE *f = fopen((const char *)arg, "r");
    if (f == NULL) {
        fprintf(stderr, "circuitpython: cannot open %s\r\n", (const char *)arg);
        return NULL;
    }
    char line[600];             // a dump names a file, its path long on Windows
    while (fgets(line, sizeof(line), f) != NULL) {
        int ms;
        if (sscanf(line, "sleep %d", &ms) == 1) {
            struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
            nanosleep(&t, NULL);
        } else if (line[strspn(line, " \t\r\n")] != '\0' && !panel_command(line)) {
            fprintf(stderr, "circuitpython: key script: not a panel command: %s", line);
        }
    }
    fclose(f);
    return NULL;
}

void desktop_sdl_start_script(void) {
    static bool started;
    const char *path = getenv("CIRCUITPY_SDL_KEYS");
    if (path == NULL || started) {
        return;
    }
    started = true;
    pthread_t thread;
    pthread_create(&thread, NULL, _key_script, (void *)path);
    pthread_detach(thread);
}

bool desktop_sdl_frame(uint16_t *width, uint16_t *height, const uint16_t **pixels) {
    if (_pixels == NULL) {
        return false;
    }
    *width = (uint16_t)_width;
    *height = (uint16_t)_height;
    *pixels = _pixels;
    return true;
}

void desktop_sdl_configure(int scale, bool fullscreen) {
    _scale = scale > 0 ? scale : 1;
    _fullscreen = fullscreen;
}

bool desktop_sdl_init(uint32_t subsystems) {
    SDL_SetMainReady();                                 // the port has its own main()
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");      // the port owns SIGINT and SIGTERM
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");    // whole pixels
    if (SDL_WasInit(subsystems) == subsystems) {
        return true;
    }
    if (SDL_InitSubSystem(subsystems) != 0) {
        fprintf(stderr, "circuitpython: %s\r\n", SDL_GetError());
        return false;
    }
    return true;
}

static bool _open(int width, int height) {
    if (_window != NULL) {
        return true;
    }
    if (!desktop_sdl_init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)) {
        return false;
    }
    Uint32 flags = SDL_WINDOW_RESIZABLE | (_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    _window = SDL_CreateWindow("CircuitPython - " MICROPY_HW_BOARD_NAME, SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, width * _scale, height * _scale, flags);
    if (_window != NULL) {
        _renderer = SDL_CreateRenderer(_window, -1, 0);
        if (_renderer == NULL) {
            _renderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_SOFTWARE);
        }
    }
    if (_renderer == NULL) {
        fprintf(stderr, "circuitpython: no display: %s\r\n", SDL_GetError());
        if (_window != NULL) {
            SDL_DestroyWindow(_window);
            _window = NULL;
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    SDL_ShowCursor(SDL_DISABLE);
    desktop_sdl_start_script();
    return true;
}

bool desktop_sdl_show(uint16_t *pixels, int width, int height) {
    if (!_open(width, height)) {
        return false;
    }
    if (_texture != NULL && (width != _width || height != _height)) {
        SDL_DestroyTexture(_texture);
        _texture = NULL;
    }
    if (_texture == NULL) {
        _texture = SDL_CreateTexture(_renderer, SDL_PIXELFORMAT_RGB565,
            SDL_TEXTUREACCESS_STREAMING, width, height);
        if (_texture == NULL) {
            fprintf(stderr, "circuitpython: no display: %s\r\n", SDL_GetError());
            return false;
        }
    }
    SDL_RenderSetLogicalSize(_renderer, width, height);
    SDL_RenderSetIntegerScale(_renderer, SDL_TRUE);
    bool was_showing = _pixels != NULL;
    free(_shown);
    _shown = malloc((size_t)width * height * 2);
    _pixels = pixels;
    _width = width;
    _height = height;
    if (!was_showing) {
        // Frames and events are handled in the background task, which runs with the tick.
        supervisor_enable_tick();
    }
    desktop_sdl_present();
    return true;
}

void desktop_sdl_hide(void) {
    if (_pixels == NULL) {
        return;
    }
    _pixels = NULL;
    free(_shown);
    _shown = NULL;
    supervisor_disable_tick();
    SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
    SDL_RenderClear(_renderer);
    SDL_RenderPresent(_renderer);
}

static float _brightness = 1.0f;

void desktop_sdl_set_brightness(float brightness) {
    _brightness = brightness;
    _stale = true;
}

float desktop_sdl_brightness(void) {
    return _brightness;
}

void desktop_sdl_present(void) {
    if (_pixels == NULL) {
        return;
    }
    SDL_UpdateTexture(_texture, NULL, _pixels, _width * 2);
    if (_shown != NULL) {
        memcpy(_shown, _pixels, (size_t)_width * _height * 2);
    }
    uint8_t level = (uint8_t)(_brightness * 255 + 0.5f);
    SDL_SetTextureColorMod(_texture, level, level, level);
    SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
    SDL_RenderClear(_renderer);
    SDL_RenderCopy(_renderer, _texture, NULL, NULL);
    SDL_RenderPresent(_renderer);
    _last_frame_ms = desktop_host_ms();
    _stale = false;
    if (__atomic_exchange_n(&_screenshot_requested, 0, __ATOMIC_ACQ_REL)) {
        _save_screenshot();
    }
}

void desktop_sdl_request_screenshot(void) {
    __atomic_store_n(&_screenshot_requested, 1, __ATOMIC_RELEASE);
}

// The left button on the display is a touch there. SDL hands the renderer's logical pixels; the
// letterbox around the picture is no touch, though a drag from the picture stays at its edge.
static void _mouse_touch(const SDL_Event *e) {
    static bool held;
    if (_window == NULL || _pixels == NULL) {
        return;
    }
    bool down = held;
    int x, y;
    if (e->type == SDL_MOUSEMOTION) {
        x = e->motion.x;
        y = e->motion.y;
    } else {
        if (e->button.button != SDL_BUTTON_LEFT) {
            return;
        }
        x = e->button.x;
        y = e->button.y;
        down = e->type == SDL_MOUSEBUTTONDOWN;
        if (down && (x < 0 || y < 0 || x >= _width || y >= _height)) {
            return;
        }
    }
    if (!down && !held) {
        return;
    }
    held = down;
    x = x < 0 ? 0 : x >= _width ? _width - 1 : x;
    y = y < 0 ? 0 : y >= _height ? _height - 1 : y;
    touch_set(TOUCH_MAIN_DISPLAY, down, x, y);
}

void desktop_sdl_task(void) {
    panel_task();
    if (_window == NULL && !panel_open()) {
        return;
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (panel_event(&event)) {
            continue;
        }
        int button = -1;
        bool pressed = false;
        switch (event.type) {
            case SDL_QUIT:
                desktop_power_off(0);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (event.key.repeat) {
                    break;
                }
                pressed = event.type == SDL_KEYDOWN;
                if (pressed && event.key.keysym.sym == SDLK_F12) {
                    desktop_sdl_request_screenshot();
                    break;
                }
                if (midi_keyboard_key(event.key.keysym.scancode, pressed)) {
                    break;
                }
                button = gamepad_key_button(event.key.keysym.sym);
                if (button < 0 && parts_key_button(event.key.keysym.sym) != NULL) {
                    parts_press(parts_key_button(event.key.keysym.sym), pressed);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEMOTION:
                _mouse_touch(&event);
                break;
            case SDL_WINDOWEVENT:
                // The display's window (the panel takes its own events): no cursor over it, unless
                // a touchscreen reads the mouse there.
                if (event.window.event == SDL_WINDOWEVENT_ENTER) {
                    SDL_ShowCursor(touch_display_bound(TOUCH_MAIN_DISPLAY) ? SDL_ENABLE : SDL_DISABLE);
                } else if (event.window.event == SDL_WINDOWEVENT_EXPOSED
                           || event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    _stale = true;
                } else if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    // SDL quits only when the last window closes, and the panel's may be open.
                    desktop_power_off(0);
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                SDL_GameControllerOpen(event.cdevice.which);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                pressed = event.type == SDL_CONTROLLERBUTTONDOWN;
                button = _pad_button(event.cbutton.button);
                break;
            default:
                break;
        }
        if (button >= 0) {
            gamepad_press(button, pressed);
        }
    }
    // A refresh presents its frame; this catches the window's own needs and pixels written
    // without one. While they keep changing they are compared at every tick, so a frame shows as
    // soon as it is drawn; a still picture is compared once a frame.
    static uint64_t last_check_ms;
    static uint64_t last_change_ms;
    uint64_t now = desktop_host_ms();
    uint64_t every = now - last_change_ms < CHANGING_MS ? 0 : FRAME_MS;
    if (_pixels == NULL || now - _last_frame_ms < FRAME_MS || now - last_check_ms < every) {
        return;
    }
    last_check_ms = now;
    bool changed = _shown != NULL && memcmp(_shown, _pixels, (size_t)_width * _height * 2) != 0;
    if (changed) {
        last_change_ms = now;
    }
    if (changed || _stale || __atomic_load_n(&_screenshot_requested, __ATOMIC_ACQUIRE)) {
        desktop_sdl_present();
    }
}
