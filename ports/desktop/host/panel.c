// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared-bindings/displayio/Bitmap.h"
#include "shared-module/fontio/BuiltinFont.h"
#include "supervisor/shared/tick.h"

#include "api_json.h"
#include "gamepad.h"
#include "i2c_virtual.h"
#include "desktop_pins.h"
#include "desktop_port.h"
#include "neopixel_emu.h"
#include "parts.h"
#include "sdl_host.h"
#include "vdisplay.h"

extern const fontio_builtinfont_t supervisor_terminal_font;

#define FRAME_MS 16
#define WIDTH 760
#define PAD 10
#define ROW 32
#define SCALE 2                     // the terminal font's 6x12 glyphs, twice as large
#define CTRL_X 150
#define CTRL_W 260
#define VALUE_X 424
#define COMMAND 0x50414e            // an SDL user event carrying a key script line
#define GAMEPAD_H 176
#define DPAD 48                     // a D-pad arm's side
#define FACE_R 26                   // a face button's radius

static SDL_Window *_window;
static SDL_Renderer *_renderer;
static bool _failed;                // no window here (a headless run): the trace still works
static bool _hidden;                // the user closed it
static bool _dirty;
static uint64_t _last_frame_ms;
static part_t *_held;               // a button or touch pad the mouse holds
static part_t *_drag;               // a slider the mouse drags
static int _displays_top;           // where the virtual displays start, under the NeoPixels
static SDL_Texture *_display_textures[VDISPLAYS_MAX];
static int _gamepad_top;            // where the gamepad starts, under the displays
static int _parts_top;              // where the rows start, under the gamepad
static int _held_button = -1;       // a gamepad button the mouse holds

void panel_changed(void) {
    _dirty = true;
}

void panel_part_added(void) {
    if (_hidden) {
        _hidden = false;
        if (_window != NULL) {
            SDL_ShowWindow(_window);
        }
    }
    _dirty = true;
}

// What the window's close button does: hidden until a part is added.
static void _close(void) {
    if (_window != NULL) {
        SDL_HideWindow(_window);
    }
    _hidden = true;
}

bool panel_open(void) {
    return _window != NULL && !_hidden;
}

// ---- drawing ----

static void _colour(uint32_t rgb) {
    SDL_SetRenderDrawColor(_renderer, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 255);
}

static void _fill(int x, int y, int w, int h, uint32_t rgb) {
    SDL_Rect r = { x, y, w, h };
    _colour(rgb);
    SDL_RenderFillRect(_renderer, &r);
}

static void _frame(int x, int y, int w, int h, uint32_t rgb) {
    SDL_Rect r = { x, y, w, h };
    _colour(rgb);
    SDL_RenderDrawRect(_renderer, &r);
}

static void _text_scaled(int x, int y, const char *s, uint32_t rgb, int scale) {
    const fontio_builtinfont_t *font = &supervisor_terminal_font;
    int per_row = font->bitmap->width / font->width;
    _colour(rgb);
    for (; *s != '\0'; s++, x += font->width * scale) {
        uint8_t glyph = fontio_builtinfont_get_glyph_index(font, (unsigned char)*s);
        int gx = (glyph % per_row) * font->width;
        int gy = (glyph / per_row) * font->height;
        for (int py = 0; py < font->height; py++) {
            for (int px = 0; px < font->width; px++) {
                if (common_hal_displayio_bitmap_get_pixel((displayio_bitmap_t *)font->bitmap, gx + px, gy + py)) {
                    SDL_Rect dot = { x + px * scale, y + py * scale, scale, scale };
                    SDL_RenderFillRect(_renderer, &dot);
                }
            }
        }
    }
}

static void _text(int x, int y, const char *s, uint32_t rgb) {
    _text_scaled(x, y, s, rgb, SCALE);
}

// Centred on x, at most max characters.
static void _text_centred(int x, int y, const char *s, uint32_t rgb, int scale, int max) {
    char clipped[16];
    snprintf(clipped, sizeof(clipped), "%.*s", max < 15 ? max : 15, s);
    _text_scaled(x - (int)strlen(clipped) * supervisor_terminal_font.width * scale / 2, y, clipped, rgb, scale);
}

static void _disc(int cx, int cy, int r, uint32_t rgb) {
    _colour(rgb);
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)SDL_sqrt((double)(r * r - dy * dy));
        SDL_RenderDrawLine(_renderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

// A virtual I2C device's channel behind a sensor part, and its value now.
static const i2c_virtual_channel_t *_channel(const part_t *p) {
    return &((const i2c_virtual_device_t *)p->device)->model->channels[p->channel];
}

static double _sensor_value(const part_t *p) {
    return ((const i2c_virtual_device_t *)p->device)->value[p->channel];
}

// A LED's light looks to the eye much brighter than its value on a screen: 1/2.2 gamma.
static uint8_t _shade(int v) {
    static uint8_t table[256];
    static bool made;
    if (!made) {
        for (int i = 0; i < 256; i++) {
            // Doubles spelled out: gcc builds the port with single-precision constants.
            table[i] = (uint8_t)(SDL_pow((double)i / (double)255, (double)1 / (double)2.2) * (double)255 + (double)0.5);
        }
        made = true;
    }
    return table[v > 255 ? 255 : v < 0 ? 0 : v];
}

static uint32_t _dim(uint32_t rgb, uint32_t level) {   // level 0-65535
    uint32_t r = ((rgb >> 16) & 0xff) * level / 65535, g = ((rgb >> 8) & 0xff) * level / 65535, b = (rgb & 0xff) * level / 65535;
    return ((uint32_t)_shade(r) << 16) | ((uint32_t)_shade(g) << 8) | _shade(b);
}

static int _cell(uint16_t cols) {
    int c = (WIDTH - 2 * PAD) / (cols > 0 ? cols : 1);
    return c > 24 ? 24 : c < 3 ? 3 : c;
}

// A display as large as the panel's width and a third of a screen's height allow, 1-3 times.
static int _display_scale(const vdisplay_t *d) {
    int s = (WIDTH - 2 * PAD) / d->width;
    if (360 / d->height < s) {
        s = 360 / d->height;
    }
    return s < 1 ? 1 : s > 3 ? 3 : s;
}

static void _draw_displays(void) {
    int y = _displays_top;
    for (int i = 0; i < vdisplay_count(); i++) {
        vdisplay_t *d = vdisplay_at(i);
        int s = _display_scale(d);
        char title[48];
        snprintf(title, sizeof(title), "%s %ux%u%s", d->name, (unsigned)d->width, (unsigned)d->height, d->on ? "" : " off");
        _text(PAD, y + 4, title, 0xd0d0d0);
        y += ROW;
        int tw = 0, th = 0;
        if (_display_textures[i] != NULL && SDL_QueryTexture(_display_textures[i], NULL, NULL, &tw, &th) == 0
            && (tw != d->width || th != d->height)) {
            // The display was resized.
            SDL_DestroyTexture(_display_textures[i]);
            _display_textures[i] = NULL;
        }
        if (_display_textures[i] == NULL) {
            _display_textures[i] = SDL_CreateTexture(_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, d->width, d->height);
            d->dirty = true;
        }
        SDL_Rect r = { PAD, y, d->width * s, d->height * s };
        if (d->on && _display_textures[i] != NULL) {
            if (d->dirty) {
                SDL_UpdateTexture(_display_textures[i], NULL, d->pixels, d->width * 2);
                d->dirty = false;
            }
            SDL_RenderCopy(_renderer, _display_textures[i], NULL, &r);
        } else {
            _fill(r.x, r.y, r.w, r.h, 0x000000);
        }
        _frame(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 0x505050);
        y += d->height * s + PAD;
    }
}

static int _height(void) {
    int h = PAD;
    uint16_t cols, rows;
    bool serpentine;
    size_t count;
    const uint8_t(*rgbw)[4];
    for (size_t i = 0; neopixel_emu_strip(i, &cols, &rows, &serpentine, &count, &rgbw); i++) {
        h += rows * _cell(cols) + PAD;
    }
    _displays_top = h;
    for (int i = 0; i < vdisplay_count(); i++) {
        h += ROW + vdisplay_at(i)->height * _display_scale(vdisplay_at(i)) + PAD;
    }
    _gamepad_top = h;
    if (gamepad_shown()) {
        h += GAMEPAD_H + PAD;
    }
    _parts_top = h;
    return h + parts_count() * ROW + PAD;
}

static void _draw_neopixels(void) {
    int y0 = PAD;
    uint16_t cols, rows;
    bool serpentine;
    size_t count;
    const uint8_t(*rgbw)[4];
    for (size_t i = 0; neopixel_emu_strip(i, &cols, &rows, &serpentine, &count, &rgbw); i++) {
        int c = _cell(cols);
        int gap = c >= 8 ? c / 6 : 0;
        for (size_t p = 0; p < count; p++) {
            int row = (int)(p / cols);
            int col = (int)(p % cols);
            if (serpentine && (row & 1)) {
                col = cols - 1 - col;
            }
            int x = PAD + col * c + gap / 2, y = y0 + row * c + gap / 2;
            _frame(x, y, c - gap, c - gap, 0x303030);
            uint32_t rgb = ((uint32_t)_shade(rgbw[p][0] + rgbw[p][3]) << 16)
                | ((uint32_t)_shade(rgbw[p][1] + rgbw[p][3]) << 8) | _shade(rgbw[p][2] + rgbw[p][3]);
            _fill(x + 1, y + 1, c - gap - 2, c - gap - 2, rgb);
        }
        y0 += rows * c + PAD;
    }
}

static void _draw_part(const part_t *p, int y) {
    char value[48] = "";
    bool output = false, level = false;
    desktop_virtual_pin(p->pin, &output, &level);
    _text(PAD, y + 4, p->name, 0xd0d0d0);
    int x = CTRL_X, cy = y + 4;
    switch (p->kind) {
        case PART_BUTTON:
            _fill(x, cy, 80, 24, p->on ? 0x909090 : 0x303030);
            _frame(x, cy, 80, 24, 0xa0a0a0);
            snprintf(value, sizeof(value), "%s  %s", p->on ? "pressed" : "released", level ? "high" : "low");
            break;
        case PART_SWITCH:
            _frame(x, cy, 60, 24, 0xa0a0a0);
            _fill(p->on ? x + 30 : x + 2, cy + 2, 28, 20, p->on ? 0x30c030 : 0x606060);
            snprintf(value, sizeof(value), "%s  %s", p->on ? "on" : "off", level ? "high" : "low");
            break;
        case PART_LED:
            _fill(x, cy, 24, 24, _dim(p->colour, p->on ? 65535 : 8000));
            snprintf(value, sizeof(value), "%s", output ? (p->on ? "on" : "off") : "input");
            break;
        case PART_TOUCH:
            _fill(x, cy, 80, 24, p->on ? 0xc0a040 : 0x303030);
            _frame(x, cy, 80, 24, 0xa0a0a0);
            snprintf(value, sizeof(value), "%s", p->on ? "touched" : "not touched");
            break;
        case PART_ANALOG: {
            unsigned mv = (unsigned)((uint32_t)p->value * 3300 / 65535);
            if (p->output) {
                _frame(x, cy, CTRL_W, 24, 0xa0a0a0);
                _fill(x + 1, cy + 1, (int)((uint32_t)(CTRL_W - 2) * p->value / 65535), 22, 0x3070d0);
            } else {
                _fill(x, cy + 10, CTRL_W, 4, 0x606060);
                _fill(x + (int)((uint32_t)(CTRL_W - 10) * p->value / 65535), cy, 10, 24, 0xd0d0d0);
            }
            snprintf(value, sizeof(value), "%5u %u.%03u V %s", (unsigned)p->value, mv / 1000, mv % 1000, p->output ? "out" : "in");
            break;
        }
        case PART_SPEAKER:
            _frame(x, cy, CTRL_W, 24, 0xa0a0a0);
            _fill(x + 1, cy + 1, (int)((uint32_t)(CTRL_W - 2) * (p->on ? p->peak : 0) / 32767), 22, 0x30c060);
            if (p->on) {
                snprintf(value, sizeof(value), "%lu Hz %u-bit %s", (unsigned long)p->sample_rate, (unsigned)p->bits,
                    p->channels == 1 ? "mono" : "stereo");
            } else {
                snprintf(value, sizeof(value), "stopped");
            }
            break;
        case PART_SHIFTREG: {
            // The gamepad's buttons it shifts out, those held lit.
            int n = 0;
            for (int k = 0; k < p->key_count && n < (int)sizeof(value) - 10; k++) {
                if (gamepad_pressed(p->keys[k])) {
                    n += snprintf(value + n, sizeof(value) - n, "%s ", gamepad_name(p->keys[k]));
                }
            }
            if (n == 0) {
                snprintf(value, sizeof(value), "%u buttons, none held", (unsigned)p->key_count);
            }
            break;
        }
        case PART_SENSOR: {
            const i2c_virtual_channel_t *ch = _channel(p);
            double v = _sensor_value(p);
            _fill(x, cy + 10, CTRL_W, 4, 0x606060);
            _fill(x + (int)((CTRL_W - 10) * (v - ch->min) / (ch->max - ch->min)), cy, 10, 24, 0xd0d0d0);
            snprintf(value, sizeof(value), "%.*f %s", ch->decimals, v, ch->unit);
            break;
        }
        case PART_IR:
            // A click sends the code; it shows as sent until the next.
            _fill(x, cy, 80, 24, 0x303030);
            _frame(x, cy, 80, 24, 0xa0a0a0);
            _text(x + 16, cy, "send", 0xffffff);
            snprintf(value, sizeof(value), "%s%u,%u,%u,%u", p->ir_sent ? "sent " : "", p->ir_code[0], p->ir_code[1],
                p->ir_code[2], p->ir_code[3]);
            break;
        case PART_ENCODER:
            _fill(x, cy, 40, 24, 0x303030);
            _frame(x, cy, 40, 24, 0xa0a0a0);
            _text(x + 14, cy, "-", 0xffffff);
            _fill(x + 50, cy, 40, 24, 0x303030);
            _frame(x + 50, cy, 40, 24, 0xa0a0a0);
            _text(x + 64, cy, "+", 0xffffff);
            snprintf(value, sizeof(value), "%ld detents", (long)(p->steps / 4));
            break;
        case PART_MIC:
            if (p->source == MIC_HOST) {
                snprintf(value, sizeof(value), "the computer's microphone");
            } else {
                _fill(x, cy + 10, CTRL_W, 4, 0x606060);
                _fill(x + (int)((uint32_t)(CTRL_W - 10) * p->value / 65535), cy, 10, 24, 0xd0d0d0);
                unsigned pct = (unsigned)((uint32_t)p->value * 100 / 65535);
                if (p->source == MIC_NOISE) {
                    snprintf(value, sizeof(value), "noise %u%%", pct);
                } else {
                    snprintf(value, sizeof(value), "tone %lu Hz %u%%", (unsigned long)p->tone_hz, pct);
                }
            }
            break;
        case PART_PWM: {
            uint32_t pulse_us = parts_pulse_us(p->duty, p->frequency);
            int angle = parts_servo_angle(pulse_us);
            if (p->servo) {
                _fill(x, cy + 10, CTRL_W, 4, 0x606060);
                if (p->pwm_active) {
                    _fill(x + angle * (CTRL_W - 6) / 180, cy, 6, 24, 0xd0d0d0);
                }
            } else {
                _fill(x, cy, 24, 24, _dim(p->colour, p->pwm_active ? p->duty : 0));
            }
            if (!p->pwm_active) {
                snprintf(value, sizeof(value), "off");
            } else if (p->servo) {
                snprintf(value, sizeof(value), "%lu us %d deg %lu Hz", (unsigned long)pulse_us, angle,
                    (unsigned long)p->frequency);
            } else {
                unsigned tenths = (unsigned)(((uint32_t)p->duty * 1000 + 32767) / 65535);
                snprintf(value, sizeof(value), "%u.%u%% %lu Hz %lu us", tenths / 10, tenths % 10,
                    (unsigned long)p->frequency, (unsigned long)pulse_us);
            }
            break;
        }
    }
    _text(VALUE_X, y + 4, value, 0xffffff);
}

// A handheld's face, PicoPad's: the D-pad left, SELECT and START between, Y X over B A right.
// L, R and MENU are keys only. False for a button not drawn.
static bool _gamepad_rect(int button, SDL_Rect *r, bool *round) {
    int cy = _gamepad_top + 88, dx = PAD + 110, fx = WIDTH - PAD - 100;
    *round = false;
    switch (button) {
        case DESKTOP_BUTTON_UP:
            *r = (SDL_Rect) { dx - DPAD / 2, cy - DPAD * 3 / 2, DPAD, DPAD };
            return true;
        case DESKTOP_BUTTON_DOWN:
            *r = (SDL_Rect) { dx - DPAD / 2, cy + DPAD / 2, DPAD, DPAD };
            return true;
        case DESKTOP_BUTTON_LEFT:
            *r = (SDL_Rect) { dx - DPAD * 3 / 2, cy - DPAD / 2, DPAD, DPAD };
            return true;
        case DESKTOP_BUTTON_RIGHT:
            *r = (SDL_Rect) { dx + DPAD / 2, cy - DPAD / 2, DPAD, DPAD };
            return true;
        case DESKTOP_BUTTON_SELECT:
            *r = (SDL_Rect) { WIDTH / 2 - 108, cy + 14, 96, 30 };
            return true;
        case DESKTOP_BUTTON_START:
            *r = (SDL_Rect) { WIDTH / 2 + 12, cy + 14, 96, 30 };
            return true;
        case DESKTOP_BUTTON_Y:
        case DESKTOP_BUTTON_X:
        case DESKTOP_BUTTON_B:
        case DESKTOP_BUTTON_A: {
            int x = button == DESKTOP_BUTTON_Y || button == DESKTOP_BUTTON_B ? fx - 80 : fx;
            int y = button == DESKTOP_BUTTON_Y || button == DESKTOP_BUTTON_X ? cy - 38 : cy + 38;
            *r = (SDL_Rect) { x - FACE_R, y - FACE_R, 2 * FACE_R, 2 * FACE_R };
            *round = true;
            return true;
        }
        default:
            return false;
    }
}

static int _gamepad_at(int x, int y) {
    SDL_Point point = { x, y };
    SDL_Rect r;
    bool round;
    for (int b = 0; gamepad_shown() && b < DESKTOP_BUTTON_COUNT; b++) {
        if (_gamepad_rect(b, &r, &round) && SDL_PointInRect(&point, &r)) {
            return b;
        }
    }
    return -1;
}

static void _draw_gamepad(void) {
    static const char *const arrows[] = { "^", "v", "<", ">" };
    int cy = _gamepad_top + 88;
    _fill(PAD + 110 - DPAD / 2, cy - DPAD / 2, DPAD, DPAD, 0x303030);
    for (int b = 0; b < DESKTOP_BUTTON_COUNT; b++) {
        SDL_Rect r;
        bool round;
        if (!_gamepad_rect(b, &r, &round)) {
            continue;
        }
        uint32_t fill = gamepad_pressed(b) ? 0x909090 : 0x303030;
        int mx = r.x + r.w / 2;
        const char *key = gamepad_key_name(b);
        if (round) {
            _disc(mx, r.y + r.h / 2, FACE_R, 0xa0a0a0);
            _disc(mx, r.y + r.h / 2, FACE_R - 2, fill);
            _text_centred(mx, r.y + r.h / 2 - 12, gamepad_name(b), 0xffffff, SCALE, 1);
            _text_centred(mx, r.y + r.h + 3, key, 0x909090, 1, 12);
        } else if (b <= DESKTOP_BUTTON_RIGHT) {
            _fill(r.x, r.y, r.w, r.h, fill);
            _frame(r.x, r.y, r.w, r.h, 0xa0a0a0);
            _text_centred(mx, r.y + 4, arrows[b], 0xffffff, SCALE, 1);
            _text_centred(mx, r.y + r.h - 15, key, 0x909090, 1, 8);
        } else {
            _fill(r.x, r.y, r.w, r.h, fill);
            _frame(r.x, r.y, r.w, r.h, 0xa0a0a0);
            _text_centred(mx, r.y + 3, gamepad_name(b), 0xffffff, SCALE, 7);
            _text_centred(mx, r.y + r.h + 3, key, 0x909090, 1, 15);
        }
    }
}

static void _draw(void) {
    int h = _height(), w, oh;
    SDL_GetWindowSize(_window, &w, &oh);
    if (w != WIDTH || oh != h) {
        SDL_SetWindowSize(_window, WIDTH, h);
    }
    _colour(0x181818);
    SDL_RenderClear(_renderer);
    _draw_neopixels();
    _draw_displays();
    if (gamepad_shown()) {
        _draw_gamepad();
    }
    for (int i = 0; i < parts_count(); i++) {
        _draw_part(parts_at(i), _parts_top + i * ROW);
    }
    SDL_RenderPresent(_renderer);
    _last_frame_ms = desktop_host_ms();
    _dirty = false;
}

static bool _open(void) {
    if (!desktop_sdl_init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        _failed = true;
        return false;
    }
    _window = SDL_CreateWindow("CircuitPython board", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        WIDTH, _height(), 0);
    _renderer = _window != NULL ? SDL_CreateRenderer(_window, -1, SDL_RENDERER_SOFTWARE) : NULL;
    if (_renderer == NULL) {
        if (_window != NULL) {
            SDL_DestroyWindow(_window);
            _window = NULL;
        }
        _failed = true;
        return false;
    }
    // The background task, which draws and pumps the window's events, runs with the tick.
    supervisor_enable_tick();
    desktop_sdl_start_script();
    _dirty = true;
    return true;
}

void panel_task(void) {
    parts_poll();
    if (_failed || _hidden || (parts_count() == 0 && neopixel_emu_strips() == 0 && !gamepad_shown()
                               && vdisplay_count() == 0)) {
        return;
    }
    if (_window == NULL && !_open()) {
        return;
    }
    if (_dirty && desktop_host_ms() - _last_frame_ms >= FRAME_MS) {
        _draw();
    }
}

// ---- input ----

static part_t *_part_at(int y) {
    return y >= _parts_top ? parts_at((y - _parts_top) / ROW) : NULL;
}

static void _slide(part_t *p, int x) {
    int v = (x - CTRL_X - 5) * 65535 / (CTRL_W - 10);
    v = v < 0 ? 0 : v > 65535 ? 65535 : v;
    if (p->kind == PART_SENSOR) {
        const i2c_virtual_channel_t *ch = _channel(p);
        parts_set_sensor(p, ch->min + (ch->max - ch->min) * v / 65535);
    } else {
        parts_set(p, (uint32_t)v);
    }
}

static bool _ours(uint32_t window_id) {
    return _window != NULL && window_id == SDL_GetWindowID(_window);
}

// "dump NAME PATH": what a display shows, as a PPM.
static void _dump(const char *line) {
    char name[32];
    int used = 0;
    if (sscanf(line, "dump %31s %n", name, &used) != 1 || used == 0) {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s", line + used);
    path[strcspn(path, "\r\n")] = '\0';
    vdisplay_t *d = vdisplay_by_name(name);
    if (d == NULL || !vdisplay_dump(d, path)) {
        fprintf(stderr, "circuitpython: panel: cannot dump %s to %s\r\n", name, path);
    } else {
        fprintf(stderr, "circuitpython: dumped %s to %s\r\n", name, path);
    }
}

bool panel_apply(const char *line, char *err, size_t errlen) {
    char verb[16], name[32];
    long value = 0;
    int n = sscanf(line, "%15s %31s %ld", verb, name, &value);
    if (n == 1 && strcmp(verb, "close") == 0) {
        _close();
        return true;
    }
    if (n >= 2 && (strcmp(verb, "down") == 0 || strcmp(verb, "up") == 0)) {
        // A key of the computer's keyboard, by its SDL name, as the key script plays it.
        char key[32];
        SDL_Event event = {0};
        event.type = verb[0] == 'd' ? SDL_KEYDOWN : SDL_KEYUP;
        event.key.state = event.type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
        event.key.keysym.sym = sscanf(line, "%*s %31[^\r\n]", key) == 1 ? SDL_GetKeyFromName(key) : SDLK_UNKNOWN;
        if (event.key.keysym.sym == SDLK_UNKNOWN) {
            snprintf(err, errlen, "no key %s", line + strlen(verb) + 1);
            return false;
        }
        SDL_PushEvent(&event);
        return true;
    }
    part_t *p = n >= 2 ? parts_by_name(name) : NULL;
    int button = n >= 2 && p == NULL ? gamepad_button(name) : -1;
    if (button >= 0 && (strcmp(verb, "press") == 0 || strcmp(verb, "release") == 0)) {
        gamepad_press(button, verb[0] == 'p');
    } else if (n < 2 || (strcmp(verb, "set") != 0 && strcmp(verb, "turn") != 0 && strcmp(verb, "send") != 0
                         && strcmp(verb, "press") != 0 && strcmp(verb, "release") != 0)) {
        snprintf(err, errlen, "not a panel command: %s", line);
        return false;
    } else if (p == NULL) {
        snprintf(err, errlen, "no part %s", name);
        return false;
    } else if (strcmp(verb, "set") == 0 && p->kind == PART_SENSOR) {
        char number[32];
        if (sscanf(line, "%*s %*s %31s", number) != 1) {
            snprintf(err, errlen, "set %s needs a value", name);
            return false;
        }
        parts_set_sensor(p, strtod(number, NULL));
    } else if (strcmp(verb, "set") == 0 && n == 3) {
        parts_set(p, (uint32_t)value);
    } else if (strcmp(verb, "turn") == 0 && n == 3) {
        parts_turn(p, (int)value);
    } else if (strcmp(verb, "send") == 0) {
        // "send NAME" its own code, "send NAME 255,2,247,8" another.
        char text[32] = "";
        uint8_t code[4];
        sscanf(line, "%*s %*s %31s", text);
        if (text[0] == '\0') {
            parts_ir_send(p, NULL);
        } else if (parts_ir_code(text, code)) {
            parts_ir_send(p, code);
        } else {
            snprintf(err, errlen, "bad IR code %s", text);
            return false;
        }
    } else if (strcmp(verb, "press") == 0 || strcmp(verb, "release") == 0) {
        parts_press(p, verb[0] == 'p');
    } else {
        snprintf(err, errlen, "%s %s needs a number", verb, name);
        return false;
    }
    return true;
}

static void _command(char *line) {
    char err[96];
    if (strncmp(line, "dump ", 5) == 0) {
        _dump(line);
    } else if (!panel_apply(line, err, sizeof(err))) {
        fprintf(stderr, "circuitpython: panel: %s\r\n", err);
    }
    free(line);
}

static const char *_kind_name(part_kind_t kind) {
    static const char *const names[] = {
        [PART_BUTTON] = "button", [PART_SWITCH] = "switch", [PART_LED] = "led", [PART_ANALOG] = "analog",
        [PART_PWM] = "pwm", [PART_TOUCH] = "touch", [PART_SPEAKER] = "speaker", [PART_MIC] = "mic",
        [PART_ENCODER] = "encoder", [PART_SENSOR] = "sensor", [PART_SHIFTREG] = "shiftreg", [PART_IR] = "ir",
    };
    return names[kind];
}

static void _json_name(api_buf_t *b, const char *key, const char *s) {
    api_buf_printf(b, ", \"%s\": ", key);
    api_buf_json_string(b, s, strlen(s));
}

static void _part_json(api_buf_t *b, const part_t *p) {
    bool output = false, level = false;
    api_buf_add(b, "{\"name\": ", 9);
    api_buf_json_string(b, p->name, strlen(p->name));
    _json_name(b, "kind", _kind_name(p->kind));
    if (p->kind != PART_SENSOR) {
        api_buf_printf(b, ", \"pin\": \"GPIO%u\"", (unsigned)p->pin);
        desktop_virtual_pin(p->pin, &output, &level);
    }
    switch (p->kind) {
        case PART_BUTTON:
        case PART_SWITCH:
        case PART_TOUCH:
            api_buf_printf(b, ", \"on\": %s, \"level\": %s", p->on ? "true" : "false", level ? "true" : "false");
            break;
        case PART_LED:
            api_buf_printf(b, ", \"on\": %s, \"output\": %s, \"colour\": \"%06lx\"", output && p->on ? "true" : "false",
                output ? "true" : "false", (unsigned long)p->colour);
            break;
        case PART_ANALOG:
            api_buf_printf(b, ", \"value\": %u, \"output\": %s", (unsigned)p->value, p->output ? "true" : "false");
            break;
        case PART_PWM: {
            uint32_t pulse_us = parts_pulse_us(p->duty, p->frequency);
            api_buf_printf(b, ", \"active\": %s, \"duty\": %u, \"frequency\": %lu, \"pulse_us\": %lu, \"colour\": \"%06lx\"",
                p->pwm_active ? "true" : "false", (unsigned)p->duty, (unsigned long)p->frequency,
                (unsigned long)pulse_us, (unsigned long)p->colour);
            if (p->servo) {
                api_buf_printf(b, ", \"angle\": %d", parts_servo_angle(pulse_us));
            }
            break;
        }
        case PART_SPEAKER:
            api_buf_printf(b, ", \"playing\": %s, \"sample_rate\": %lu, \"bits\": %u, \"channels\": %u, \"peak\": %u",
                p->on ? "true" : "false", (unsigned long)p->sample_rate, (unsigned)p->bits, (unsigned)p->channels,
                (unsigned)p->peak);
            break;
        case PART_MIC:
            _json_name(b, "source", p->source == MIC_HOST ? "host" : p->source == MIC_NOISE ? "noise" : "tone");
            api_buf_printf(b, ", \"value\": %u, \"tone_hz\": %lu", (unsigned)p->value, (unsigned long)p->tone_hz);
            break;
        case PART_ENCODER:
            api_buf_printf(b, ", \"position\": %ld", (long)(p->steps / 4));
            break;
        case PART_SENSOR: {
            const i2c_virtual_channel_t *ch = _channel(p);
            api_buf_printf(b, ", \"value\": %.*f", ch->decimals + 2, _sensor_value(p));
            _json_name(b, "unit", ch->unit);
            api_buf_printf(b, ", \"min\": %g, \"max\": %g", ch->min, ch->max);
            break;
        }
        case PART_SHIFTREG:
            api_buf_printf(b, ", \"latched\": %u", (unsigned)p->latched);
            break;
        case PART_IR:
            api_buf_printf(b, ", \"code\": \"%u,%u,%u,%u\", \"sent\": %s", p->ir_code[0], p->ir_code[1], p->ir_code[2],
                p->ir_code[3], p->ir_sent ? "true" : "false");
            break;
    }
    api_buf_add(b, "}", 1);
}

void panel_state_json(api_buf_t *b) {
    // The window: shown, closed by the user, or none (not opened yet, or no video here).
    api_buf_printf(b, "{\"panel\": \"%s\", ", _hidden ? "hidden" : _window != NULL ? "shown" : "none");
    api_buf_add(b, "\"parts\": [", 10);
    for (int i = 0; i < parts_count(); i++) {
        api_buf_add(b, i ? ", " : "", i ? 2 : 0);
        _part_json(b, parts_at(i));
    }
    api_buf_add(b, "], \"neopixels\": [", 17);
    for (size_t i = 0; i < neopixel_emu_strips(); i++) {
        uint16_t cols, rows;
        bool serpentine;
        size_t count;
        const uint8_t(*rgbw)[4];
        if (!neopixel_emu_strip(i, &cols, &rows, &serpentine, &count, &rgbw) || count == 0) {
            continue;
        }
        api_buf_printf(b, "%s{\"index\": %u, \"cols\": %u, \"rows\": %u, \"pixels\": [", i ? ", " : "",
            (unsigned)i, (unsigned)cols, (unsigned)rows);
        for (size_t k = 0; k < count; k++) {
            if (rgbw[k][3]) {
                api_buf_printf(b, "%s\"%02x%02x%02x%02x\"", k ? ", " : "", rgbw[k][0], rgbw[k][1], rgbw[k][2], rgbw[k][3]);
            } else {
                api_buf_printf(b, "%s\"%02x%02x%02x\"", k ? ", " : "", rgbw[k][0], rgbw[k][1], rgbw[k][2]);
            }
        }
        api_buf_add(b, "]}", 2);
    }
    api_buf_add(b, "], \"displays\": [", 16);
    uint16_t w, h;
    const uint16_t *pixels;
    bool first = true;
    if (desktop_sdl_frame(&w, &h, &pixels)) {
        api_buf_printf(b, "{\"name\": \"DISPLAY\", \"width\": %u, \"height\": %u}", (unsigned)w, (unsigned)h);
        first = false;
    }
    for (int i = 0; i < vdisplay_count(); i++) {
        const vdisplay_t *d = vdisplay_at(i);
        api_buf_printf(b, "%s{\"name\": ", first ? "" : ", ");
        api_buf_json_string(b, d->name, strlen(d->name));
        api_buf_printf(b, ", \"width\": %u, \"height\": %u, \"on\": %s}", (unsigned)d->width, (unsigned)d->height,
            d->on ? "true" : "false");
        first = false;
    }
    api_buf_add(b, "], \"gamepad\": {\"pressed\": [", 27);
    first = true;
    for (int i = 0; i < DESKTOP_BUTTON_COUNT; i++) {
        if (gamepad_pressed(i)) {
            api_buf_printf(b, "%s\"%s\"", first ? "" : ", ", gamepad_name(i));
            first = false;
        }
    }
    api_buf_add(b, "]}}", 3);
}

bool panel_event(const SDL_Event *e) {
    switch (e->type) {
        case SDL_USEREVENT:
            if (e->user.code != COMMAND) {
                return false;
            }
            _command(e->user.data1);
            return true;
        case SDL_MOUSEBUTTONDOWN: {
            if (!_ours(e->button.windowID)) {
                return false;
            }
            int button = _gamepad_at(e->button.x, e->button.y);
            if (button >= 0) {
                _held_button = button;
                gamepad_press(button, true);
                return true;
            }
            part_t *p = _part_at(e->button.y);
            if (p == NULL || e->button.x < CTRL_X || e->button.x > CTRL_X + CTRL_W) {
                return true;
            }
            if (p->kind == PART_BUTTON || p->kind == PART_TOUCH) {
                _held = p;
                parts_press(p, true);
            } else if (p->kind == PART_SWITCH) {
                parts_press(p, true);
            } else if (p->kind == PART_IR) {
                parts_ir_send(p, NULL);
            } else if (p->kind == PART_ENCODER) {
                // The - and + keys: a detent counter-clockwise or clockwise.
                parts_turn(p, e->button.x < CTRL_X + 45 ? -1 : 1);
            } else if ((p->kind == PART_ANALOG && !p->output) || (p->kind == PART_MIC && p->source != MIC_HOST)
                       || p->kind == PART_SENSOR) {
                _drag = p;
                _slide(p, e->button.x);
            }
            return true;
        }
        case SDL_MOUSEMOTION:
            if (!_ours(e->motion.windowID)) {
                return false;
            }
            if (_drag != NULL) {
                _slide(_drag, e->motion.x);
            }
            return true;
        case SDL_MOUSEBUTTONUP:
            if (!_ours(e->button.windowID)) {
                return false;
            }
            if (_held != NULL) {
                parts_press(_held, false);
            }
            if (_held_button >= 0) {
                gamepad_press(_held_button, false);
            }
            _held = NULL;
            _held_button = -1;
            _drag = NULL;
            return true;
        case SDL_MOUSEWHEEL: {
            if (!_ours(e->wheel.windowID)) {
                return false;
            }
            int x, y;
            SDL_GetMouseState(&x, &y);
            part_t *p = _part_at(y);
            if (p != NULL && p->kind == PART_ENCODER) {
                parts_turn(p, e->wheel.y);
            } else if (p != NULL && p->kind == PART_SENSOR) {
                // A hundredth of the range, Shift a thousandth.
                const i2c_virtual_channel_t *ch = _channel(p);
                double step = (ch->max - ch->min) / ((SDL_GetModState() & KMOD_SHIFT) ? 1000 : 100);
                parts_set_sensor(p, _sensor_value(p) + e->wheel.y * step);
            } else if (p != NULL && ((p->kind == PART_ANALOG && !p->output) || (p->kind == PART_MIC && p->source != MIC_HOST))) {
                int step = (SDL_GetModState() & KMOD_SHIFT) ? 1 : 1024;
                int v = p->value + e->wheel.y * step;
                parts_set(p, (uint32_t)(v < 0 ? 0 : v > 65535 ? 65535 : v));
            }
            return true;
        }
        case SDL_WINDOWEVENT:
            if (!_ours(e->window.windowID)) {
                return false;
            }
            if (e->window.event == SDL_WINDOWEVENT_ENTER) {
                // The display hides the cursor (SDL has one for all windows); the panel needs it.
                SDL_ShowCursor(SDL_ENABLE);
            } else if (e->window.event == SDL_WINDOWEVENT_CLOSE) {
                _close();
            } else if (e->window.event == SDL_WINDOWEVENT_EXPOSED) {
                _dirty = true;
            }
            return true;
        default:
            return false;
    }
}

bool panel_command(const char *line) {
    char verb[16];
    if (sscanf(line, "%15s", verb) != 1 || (strcmp(verb, "set") != 0 && strcmp(verb, "press") != 0 && strcmp(verb, "release") != 0
                                            && strcmp(verb, "turn") != 0 && strcmp(verb, "dump") != 0 && strcmp(verb, "send") != 0
                                            && strcmp(verb, "close") != 0)) {
        return false;
    }
    SDL_Event event = { 0 };
    event.type = SDL_USEREVENT;
    event.user.code = COMMAND;
    event.user.data1 = strdup(line);
    SDL_PushEvent(&event);
    desktop_wake_vm();
    return true;
}
