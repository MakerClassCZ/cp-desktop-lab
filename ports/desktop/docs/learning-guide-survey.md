# What Learning Guide code runs on the port

A survey on 2026-10-22: 200 `code.py` files picked at random (seed 2026) from the 1757 in
adafruit/Adafruit_Learning_System_Guides (commit 43bd33e6), each run for 10 seconds on the
Linux build with `--board desktop` and every library its project imports installed from the
Adafruit bundle (`vscode/libinstall.js`, 102 libraries). `tools/survey_learning_guides.py` does
it again. Only the guides' `.py` files were fetched, so a program that opens an image or a font
fails for that reason too, which counts below as a missing file.

## Result

| what happened | programs |
|---|---|
| ran (no error within 10 s, or ended cleanly) | 1 |
| a module CircuitPython has on boards, missing here | 143 |
| a pin or name the desktop board lacks | 47 |
| another error | 7 |
| needs hardware the run did not give (`--uart`) | 1 |
| a file the survey did not fetch | 1 |

A program stops at its first failure, so each count is what stops it first; a program with a
missing module may also need missing pins after it.

## What stops them

Missing modules, by programs:

| module | programs | what it is |
|---|---|---|
| `neopixel_write` | 46 | NeoPixels: the `neopixel` library writes through it |
| `pwmio` | 21 | PWM: servos, buzzers, dimmed LEDs |
| `audiobusio` | 18 | I2S audio and PDM microphones |
| `rgbmatrix` | 8 | HUB75 LED matrices |
| `fourwire` | 7 | SPI displays (ST7789, ILI9341 ...) |
| `wifi` | 5 | Wi-Fi; on the port `hostnetwork` stands in |
| `touchio`, `usb_hid` | 4 each | capacitive touch, keyboard/mouse |
| `audiopwmio` | 3 | PWM audio |
| `PIL`, `requests`, `urllib`, `datetime`, `network` | 1-3 each | CPython or MicroPython code, not CircuitPython |
| others | 1-2 each | `usb_midi`, `_bleio`, `usb_cdc`, `pulseio`, `max3421e`, `rotaryio`, `usb`, `sdcardio`, `is31fl3741` |

Missing names on the board (47): `sys.platform` (16, all through `adafruit_circuitplayground`;
fixed since: the port now names its host as CPython does), `board.I2C` (5, the desktop board
has no bus), and board pins of the board each guide was written for: `D5`, `D13`, `A0`, `LED`,
`SPEAKER_ENABLE`, `DOTSTAR_CLOCK`, `GP13`, `CHIP_SELECT_PIN`, `CAN_CS`, `TOUCH_XL` ...

The other errors are the hardware not being there: a Crickit or a Seesaw that does not answer
(4), a GPS sequence, a read-only drive the guide expects to write (Python writes need
`storage.remount`, as on a board), an LCD pin import.

## After NeoPixels and a board file (the same 200, the same day)

With `neopixel_write` and `sys.platform` in, run again on `--board desktop`, and once more with
`--board-file boards/files/feather.toml` (Feather pin names on virtual pins):

| | before | desktop | desktop + feather.toml |
|---|---|---|---|
| ran | 1 | 1 | 7 |
| missing module | 143 | 136 | 136 |
| missing name on the board | 47 | 53 | 43 |
| other error, hardware, file | 9 | 10 | 14 |

The 46 programs that stopped at `neopixel_write` got past it and mostly stopped at the next
module: `pwmio` (8), `wifi` (4), `_bleio` (3), `usb_hid` (3), or at their board's pins: `D1`
(7, a Gemma's or a Trinket's), `A1`, `NEOPIXEL`. With the Feather names three of them run (an
RGBW strip, a Circuit Playground's ring through a Crickit, a Trinkey), and so do a MOSFET driver,
a Pico data logger and a stepper driver example. The modules missing now, by programs: `pwmio`
29, `audiobusio` 20, `touchio` 19, `wifi` 9, `rgbmatrix` 8, `fourwire` 7, `usb_hid` 7.
`analogio` needs an MCP2221 (3 programs reached it with the Feather's `A` pins).

## After the board panel (2026-10-23)

With `pwmio`, `touchio` and `analogio` on the desktop board (the board panel), the same 200:

| | desktop | desktop + feather.toml |
|---|---|---|
| ran | 1 | 21 |
| missing module | 93 | 93 |
| missing name on the board | 91 | 67 |
| other error, hardware, file | 15 | 19 |

`pwmio` (29) and `touchio` (19) are gone from the missing modules. What stops programs now:
`audiobusio` 21, `wifi` 10, `rgbmatrix` 8, `fourwire` 7, `usb_hid` 7; among the names
`SLIDE_SWITCH` (17, the `adafruit_circuitplayground` library: a run with
`circuitplayground.toml` would move those) and `D1` (10, Gemma and Trinket code).

## What would help most

1. Done since: `neopixel_write` (a window of NeoPixels) and `--board-file` (other boards' pin
   names); more board files (Gemma/Trinket `D0`-`D4`, Pico `GP0`-`GP28`) would move a few more.
2. `pwmio` (duty cycle and frequency shown and traced) and `touchio` as virtual pins: 48
   programs stop there now. `analogio` on the desktop board as virtual inputs.
3. `fourwire` and `busdisplay` over the FT232H's SPI: real SPI displays, and on the desktop the
   display it already has.
4. Audio (`audiobusio`, `audiopwmio`) is a larger piece: the port plays sound through SDL with
   `audioio` already, the I2S and PWM classes could play through the same.

Almost every sampled program is written for a board with LEDs, sound or its own pins. Each gap
filled moves the first failure of many programs but makes few run on its own: most meet the next
missing module, and many a sensor that is not there, which on the port means an FT232H or an
MCP2221 with the part wired to it.

## The whole repository (2026-10-23)

Every `code.py` of the same commit, 1757, with `audiobusio` in too, run as above with
`JOBS=8`: once with `boards/files/feather.toml`, once with `circuitplayground.toml`. The 236
libraries they import that the bundle has were installed (80 names are not in it: the projects'
own modules, CPython ones).

| | feather.toml | circuitplayground.toml | either |
|---|---|---|---|
| ran | 231 | 202 | 262 (15 %) |

What stops the rest first (with the board file that gets it further):

| | programs |
|---|---|
| `wifi` | 122, and 43 more that stop on the guide's `settings.toml` |
| `SLIDE_SWITCH` / `ACCELEROMETER_SCL` (the Circuit Playground library: its accelerometer on I2C) | 85 / 89 |
| a file the survey did not fetch (images, fonts, sounds) | 78 |
| `_bleio` | 68 |
| `fourwire`, `i2cdisplaybus` (SPI and I2C displays) | 67, 15 |
| `usb_hid`, `usb_midi`, `usb` | 66, 29, 19 |
| `audiopwmio` | 62 |
| `board.SCL`, `board.I2C`, `STEMMA_I2C` (the desktop board has no I2C) | 61, 57, 10 |
| `D0`-`D4` (Gemma, Trinket) | 119 |
| `rgbmatrix` | 39 |
| `pulseio` | 37 |
| `picodvi` | 28 |
| `rotaryio` | 23 |
| `alarm` | 16 |
| `GP0`-`GP28` (Pico) | about 20 |

## After the second round (2026-10-23)

With `audiopwmio`, `rotaryio`, `usb_hid`, `usb_midi`, the Trinket and Pico board files and the
virtual I2C bus (LIS3DH, BME280, MCP9808, DS3231, LC709203F), the whole repository again, once
per board file (feather.toml still with its LIS3DH at 0x19):

| board file | ran | before |
|---|---|---|
| feather.toml | 306 | 231 |
| circuitplayground.toml | 339 | 202 |
| trinket.toml | 269 | |
| pico.toml | 143 | |
| any of them | 551 (31 %) | 262 (15 %) |

What stops the rest first: `wifi` 118 (and 37 more that stop on their own `settings.toml`
check), a file the survey did not fetch 86, `fourwire` 70, `_bleio` 70, a seesaw at 0x49 (the
Crickit) 56, `rgbmatrix` 39, `pulseio` 38, `picodvi` 29, `busio.SPI` on the desktop board 28,
`from adafruit_circuitplayground import cp` 26 (the library picks the board by
`sys.platform`), `i2cdisplaybus` 16, `alarm` 16.

## Each program with its own board (2026-10-23)

From here a program runs once, with the board file it is written for (`board_for`: the Circuit
Playground library or its names, `GPn` or "pico", "trinket"/"gemma" in the path, else the
Feather), and only the programs that have not run yet run again (`seed`, then `run` with the
board files' directory). With `wifi` and made-up settings (`SETTINGS=1`):

| board file | programs | ran |
|---|---|---|
| feather | 1393 | 389 |
| circuitplayground | 232 | 111 |
| pico | 81 | 45 |
| trinket | 51 | 30 |
| all | 1757 | 575 (33 %) |

`wifi` no longer stops anything; of the 158 programs it or its settings stopped, 15 run, and the
rest meet the next thing: `NEOPIXEL_POWER` 35 (ESP32-S2/S3 Feathers), a file the survey did not
fetch 18, a setting the made-up ones lack 11, an AirLift (`ESP_CS`) 10. What stops programs
now: a file not fetched 101, `fourwire` 71, `_bleio` 71, a seesaw at 0x49 54, `rgbmatrix` 47,
`pulseio` 38, `NEOPIXEL_POWER` 35, `picodvi` 29, `SCK` 26 and `busio.SPI` on the desktop board
17, `from adafruit_circuitplayground import cp` 26.

With `NEOPIXEL_POWER`, `EXTERNAL_POWER` and `D4` in feather.toml, and the data files of the 101
projects that missed one (`files`: their folders added to the sparse checkout, 261 files, no
lib/, nothing over 8 MB), 619 run. Their next stops: `BATTERY` 35 (the same ESP32-S2/S3 Feather
programs), `board.SPI` 32 and `SCK` 26, a file still missing 26 (a library's, or one the program
writes first).

With the virtual displays and SPI bus, the MagTag board file (MagTag code runs with it), the
Circuit Playground's TX on its A7 pad, the Feather's SPI pins as pins and the data files of 64
more projects: 680 run (feather 454, circuitplayground 123, pico 49, trinket 38, magtag 16).
What stops the rest first: `_bleio` 74, a seesaw at 0x49 55, `rgbmatrix` 47, `pulseio` 38, an
invalid pin 32, a missing file 30, an AirLift (`ESP_BUSY`, `ESP_CS`) 43, `picodvi` 29,
`from adafruit_circuitplayground import cp` 27, other boards' buttons (`BUTTON_A` 26).

With `sys.platform` and `os.uname().machine` from a board file's `[board]`, board files for the
CLUE (its five sensors modelled on the virtual I2C bus), the PyBadge and PyGamer (their buttons
behind a shift register), the MatrixPortal (`rgbmatrix` on the panel), `pulseio` (an IR receiver
on the panel, `PulseOut` in the bus trace) and the data files of 58 more projects: 758 run
(feather 463, circuitplayground 157, clue 16, magtag 16, matrixportal 13, pico 49, trinket 40,
pybadge 2, pygamer 2). What stops the rest first: `_bleio` 80, a seesaw at 0x49 56, a missing
file 34, an invalid pin 33, an AirLift (`ESP_BUSY`, `ESP_CS`) 43, `picodvi` 29, pins of boards
with no board file (`D8` 20, `D0` 19, `D7` 12), `usb` 19, `alarm` 19, `PIL` 18 (CPython code),
`espcamera` 17, `sdcardio` 14.

With a Crickit (a seesaw at 0x49, product 9999, so `adafruit_crickit` and `adafruit_seesaw`
with the Crickit's pins run) on the Feather and Circuit Playground buses and the data files of
45 more projects: 786 run (feather 485, circuitplayground 163). Of the programs that use a
seesaw, 27 run; the rest stop at other seesaw boards (0x36 9, 0x30 3), a CLUE's or Circuit
Playground Bluefruit's names (`BUTTON_A` 7) or a file. What stops programs first now: `_bleio`
84, a missing file 41, an invalid pin 33, an AirLift (`ESP_BUSY`, `ESP_CS`) 43, `picodvi` 29.

The quick classes (2026-10-23): a display no board file names appears when the program makes it
(a TFT on any fourwire cs pin, its rotation turned back; an SSD1306 at any I2C display address),
`busio.UART` with no `--uart` is a virtual port, `TX`/`RX`/`SCL`/`SDA` are plain pins too,
`board.DISPLAY.brightness` dims the window, the survey makes up every `getenv` name: 834 run,
843 with the data files of 62 more projects. A driver's `I2CDevice` finding the modelled sensor
at its address (0x6a, 0x39, 0x44...): 852 (feather 531, circuitplayground 167, pico 54, trinket
42, clue 23, magtag 16, matrixportal 15).

What stops the rest first, by class: pins of boards with no board file ~330 over some 30 boards
(`D0`-`D8` alone ~85, the PyPortal's names, `DOTSTAR_*`, `EXTERNAL_*`), I2C devices with no model
~115 over 35 addresses (IS31FL3741 0x30 12, seesaw boards 0x36 11, HT16K33 0x70 10), `_bleio` 84,
an AirLift (`ESP_BUSY`, `ESP_CS`) 50, a file 44 (a library's, or one the program writes first),
modules no port of this kind has (`picodvi`, `usb`, `alarm`, `espcamera`, `sdcardio`, `PIL`) ~115.
