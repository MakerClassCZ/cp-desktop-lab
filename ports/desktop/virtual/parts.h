// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The virtual parts on the virtual board's pins: what the board panel (panel.c) shows and the
// user moves, and what common-hal reads and writes. Everything runs on the VM thread.

typedef enum {
    PART_BUTTON,        // a digitalio input, pressed while held
    PART_SWITCH,        // a digitalio input, toggled
    PART_LED,           // a digitalio output shown as a lamp
    PART_ANALOG,        // analogio: AnalogIn reads a slider, AnalogOut shows a bar
    PART_PWM,           // pwmio.PWMOut shown as a lamp at its duty cycle
    PART_TOUCH,         // touchio.TouchIn, touched while held
    PART_SPEAKER,       // audiobusio.I2SOut, playing through the computer
    PART_MIC,           // audiobusio.PDMIn, recording the panel's generator or the computer's microphone
    PART_ENCODER,       // rotaryio.IncrementalEncoder on its first pin, turned a detent at a time
    PART_SENSOR,        // a value of a virtual I2C device (i2c_virtual.c), on no pin
    PART_SHIFTREG,      // a shift register of buttons (keypad.ShiftRegisterKeys) on its data pin
    PART_IR,            // an IR receiver (pulseio.PulseIn): a remote's NEC code sent on a click
    PART_CC,            // a MIDI CC slider (midi_in.c), on no pin: value 0-127
} part_kind_t;

typedef enum {
    MIC_TONE,           // a sine at tone_hz, as loud as the slider
    MIC_NOISE,          // white noise, as loud as the slider
    MIC_HOST,           // the computer's microphone
} mic_source_t;

typedef struct {
    part_kind_t kind;
    uint8_t pin;
    char name[24];
    // Pulled out (board.detach, or replaced at its place): the slot stays, so what common-hal
    // holds stays valid and works again when a part of this kind is attached here.
    bool detached;
    bool automatic;     // added by parts_for_pin, not by a board file or board.attach
    // options
    bool active_high;   // a button: pressed reads high
    uint32_t colour;    // a lamp: 0xRRGGBB
    bool servo;         // a PWM: show the angle of a 1-2 ms pulse
    // state
    bool on;            // a button or touch held, a switch on, an LED lit
    bool output;        // an analog pin: an AnalogOut drives it
    uint16_t value;     // an analog pin: the slider, or what the AnalogOut wrote
    uint16_t duty;      // a PWM
    uint32_t frequency;
    bool pwm_active;
    // a speaker: what plays, and its loudest recent sample (0-32767)
    uint32_t sample_rate;
    uint8_t bits, channels;
    uint16_t peak;
    void *out;          // the speaker's audioio_audioout_obj_t while an I2SOut has it
    const char *label;  // the speaker's name in the trace: I2S unless set (PWMAUDIO)
    // an encoder: the quadrature steps turned so far, four to a detent
    int32_t steps;
    // a sensor: its device's channel
    void *device;
    uint8_t channel;
    // a shift register: its clock and latch pins, the gamepad button behind each bit, and the
    // buttons latched and the bit the clock has reached
    uint8_t clock_pin, latch_pin;
    int8_t keys[16];
    uint8_t key_count, bit;
    uint16_t latched;
    // an IR receiver: the PulseIn listening and the code a click sends
    void *pulsein;
    uint8_t ir_code[4];
    bool ir_sent;
    bool clock_level, latch_level;
    // a CC slider: its controller and channel (1-16); value is 0-127
    uint8_t cc, midi_channel;
    // a microphone; value is the level
    mic_source_t source;
    uint32_t tone_hz;
    double phase;
} part_t;

#define PARTS_MAX 48

// A part from the board file or board.attach: options as written there; NULL with *why set when
// they are wrong. The last wins: an attached part of another kind at the pin, or of the same name
// elsewhere, is detached; one of the same kind at the pin is reused with its state.
part_t *parts_add(part_kind_t kind, uint8_t pin, const char *name, const char *options, const char **why);
// The part of a kind at a pin, attached or not.
part_t *parts_find(uint8_t pin, part_kind_t kind);
// An attached part by its name.
part_t *parts_by_name(const char *name);
// The attached parts, as the panel shows them.
part_t *parts_at(int i);
int parts_count(void);
// Pulled out: what it drives on its pin let go, left out of the panel, its slot kept.
void parts_detach(part_t *part);

// The user's side, from the panel or the key script: traced, and a button or switch drives its pin.
void parts_press(part_t *part, bool down);
void parts_set(part_t *part, uint32_t value);
// A CC slider: options "74", "74 100" (its starting value), "1 channel=2". One of the same name is
// reused; at most PARTS_CC_MAX. NULL with *why set when they are wrong.
#define PARTS_CC_MAX 8
part_t *parts_add_cc(const char *name, const char *options, const char **why);
// Each CC slider's value queued as MIDI, as a run's first read gets them.
void parts_cc_send_all(void);
// A virtual I2C device's channel, named DEVICE.CHANNEL; its earlier part reused, attached again.
part_t *parts_add_sensor(void *device, int channel);
// A device pulled out: its channels' parts detached.
void parts_detach_device(void *device);
// A sensor's value in its unit, kept in its range.
void parts_set_sensor(part_t *part, double value);
// An encoder turned by detents, negative counter-clockwise.
void parts_turn(part_t *part, int detents);
// The program's side: the part of a kind at a pin, attached or not. A pin nothing was ever at gets
// one now, named as the board names it; at a pin whose part was pulled out it is added detached.
part_t *parts_for_pin(uint8_t pin, part_kind_t kind);
// The name the board gives a pin: the first key whose value it is, else GPIOn.
void parts_pin_name(uint8_t pin, char *name, size_t len);
// A virtual pin the program wrote or reconfigured: an LED on it follows.
void parts_pin_changed(uint8_t pin);
// A shift register of the gamepad's buttons: keys names them in the order they shift out, a
// pressed one high. NULL with *why set when a name is wrong.
part_t *parts_add_shiftreg(uint8_t data, uint8_t clock, uint8_t latch, const char *name, const char *keys, const char **why);
// The level a part puts on a pin the program reads (a shift register's data); false for none.
bool parts_pin_level(uint8_t pin, bool *level);
// An IR receiver gets a remote's code as NEC pulses, each byte's bits most significant first (as
// adafruit_irremote reads them back); a code of NULL sends the part's own.
void parts_ir_send(part_t *part, const uint8_t code[4]);
// "255,2,247,8" (decimal or 0x hex) into four bytes.
bool parts_ir_code(const char *text, uint8_t code[4]);
// A PWMOut the program set: shown and traced; active false when it stops.
void parts_pwm(part_t *part, uint16_t duty, uint32_t frequency, bool active);
// A pulse's width in microseconds, rounded: duty / 65535 of a period.
uint32_t parts_pulse_us(uint16_t duty, uint32_t frequency);
// A servo's arm for a pulse: 750-2250 us is 0-180 degrees, as adafruit_motor.servo drives one.
int parts_servo_angle(uint32_t pulse_us);
// After a VM: no part points into its heap any more; outputs show nothing driven.
void parts_reset(void);
// Speakers whose sample ran out are stopped and traced, their levels taken; from the SDL task.
void parts_poll(void);
// An I2SOut that started (playing) or stopped: shown and traced.
void parts_speaker(part_t *part, bool playing, uint32_t sample_rate, uint8_t bits, uint8_t channels);
