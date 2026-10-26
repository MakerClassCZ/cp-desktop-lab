# CircuitPython for Linux and Windows

CircuitPython as a program: the supervisor, the VM and the CIRCUITPY drive run in one process,
the terminal is the serial console, and SDL2 provides the display, the buttons and the sound. It
is meant for Linux handhelds (Miyoo, Anbernic and the like) and as a fast way to run
CircuitPython code on a desktop, Linux or Windows. This port is experimental.

## Build

Needs GCC 10 or newer and the SDL2 development files (`libsdl2-dev` on Debian and Ubuntu).

```
make                        # build-linux/circuitpython
make CC="gcc -m32" BUILD=build-linux-i386     # a 32-bit build on a 64-bit PC
```

Another SDL2 can be given with `SDL_CFLAGS` and `SDL_LIBS`.

The Windows program is a cross build from Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
and the MinGW development package of SDL2 from [libsdl.org](https://github.com/libsdl-org/SDL/releases):

```
make BUILD=build-windows CROSS_COMPILE=x86_64-w64-mingw32- SDL_DIR=/path/to/SDL2-2.32.8/x86_64-w64-mingw32
```

`build-windows/circuitpython.exe` runs on Windows 10 and 11 with `SDL2.dll` from the same SDL2
package beside it; the C runtime and winpthreads are built in. A crash prints an address that
`x86_64-w64-mingw32-addr2line -f -e build-windows/circuitpython.exe 0x...` resolves.

## Run

```
build-linux/circuitpython [--board NAME] [--drive PATH] [--drive-size MB] [--ram MB]
```

`--board` is `virtual` (the default: the window, the keyboard and game controllers as buttons),
`ft232h` or `mcp2221` (real pins through that chip on USB, see below).

CIRCUITPY is a directory, `./CIRCUITPY` by default. A missing directory is created on first start
with what a blank board has: `code.py`, `settings.toml` and `lib/`. Edit the files with any editor
or copy a project in, and `code.py` reloads after each change, as when a computer saves to a
board's drive. Hidden files such as editor swap files and `.git` do not trigger a reload.

Python sees the directory read-only, as on a board whose drive is connected to a computer.
`storage.remount("/", readonly=False)` in `boot.py` gives it to Python instead: Python can write,
and host edits stop reloading `code.py`. Unlike FAT, file names are case-sensitive, and
`storage.erase_filesystem()` leaves the directory as it is. `storage.map_file()` maps a file of
the directory straight into memory.

On Windows the drive is `.\CIRCUITPY` by default and any path works, `C:\Users\me\game` as well
as a relative one.

`--drive` can also name a FAT image file. A missing path ending in `.img` becomes a new image of
`--drive-size` MB, formatted like a blank board. The host reads and writes an image with mtools or
a loop mount, but only while CircuitPython is not running:

```
mcopy -i CIRCUITPY.img -o code.py ::code.py
mcopy -i CIRCUITPY.img -s -o mygame/* ::/
```

The console works as on a board: Ctrl-C interrupts, Ctrl-D reloads, any key enters the REPL.
Ctrl-\, closing the window or `board.exit()` (from code or the REPL, with an optional exit
status) ends the program. `microcontroller.reset()` restarts the process, and a
crash restarts it in safe mode with a backtrace on stderr; `addr2line -f -e
build-linux/circuitpython 0x...` turns its `circuitpython(+0x...)` lines into function names. A
second crash right after such a restart ends the program instead. On Windows a reset starts a new
process and the old one waits for it, so after sixteen resets in a row the program ends.

A desktop runs Python a hundred times faster than a microcontroller, and a virtual sensor answers
at once. A program that relies on its board's slowness for its pace runs too fast: a loop without
`time.sleep()` that plots a sensor (the CLUE's sensor plotter) samples every few microseconds and
draws a flat line. Such a program needs the sleep it should have had on the board too.

## Display, buttons and sound

`supervisor.runtime.display` is a `framebufferio.FramebufferDisplay` over an `sdl.Framebuffer`
(`board.DISPLAY` is the same object, for older code), so the console appears on screen and
`displayio`, `terminalio` and `picogame` draw into it. SDL2 picks the window system on a desktop
and KMS/DRM or fbdev on a handheld. `settings.toml` sets it up:

| key | default | meaning |
|---|---|---|
| `CIRCUITPY_DISPLAY_WIDTH`, `CIRCUITPY_DISPLAY_HEIGHT` | 320, 240 | the picture in pixels |
| `CIRCUITPY_DISPLAY_SCALE` | 2 | window size as a multiple of the picture |
| `CIRCUITPY_DISPLAY_FULLSCREEN` | false | fill the screen, whole-pixel scaling |
| `CIRCUITPY_DISPLAY_ROTATION` | 0 | 0, 90, 180 or 270 |

The buttons are pins with the PicoPad's names, `board.SW_UP` to `board.SW_MENU`, low while
pressed, so `keypad.Keys` and code written for the PicoPad work unchanged. They follow a game
controller, which is how SDL reports a handheld's own buttons, or the keyboard:

| button | key | button | key |
|---|---|---|---|
| UP, DOWN, LEFT, RIGHT | arrows | START | Enter |
| A, B | Z, X | SELECT | Backspace |
| X, Y | A, S | L, R | Q, W |
| MENU | Esc | | |

A `[gamepad]` section in a board file (below) shows them on the board panel as the PicoPad's
face, the D-pad, SELECT and START, Y X over B A, to click, each with its key; and gives a button
keys of its own, by their SDL names, in place of its defaults (a key named leaves the button it
had):

```
[gamepad]
A = "Space, Z"
UP = "W, Up"
```

`boards/files/picopad.toml` is such a file. The keys work in the panel's window as in the
display's.

`audioio.AudioOut(board.SPEAKER)` plays through SDL2 audio: samples, `audiomixer` and `synthio`.

F12 or `kill -USR1` saves a screenshot, `screenshot-NNN.bmp` or the file named by
`CIRCUITPY_SCREENSHOT`.

## Real pins with an FT232H

`--board ft232h` gives the program the pins of an FT232H breakout on USB (Adafruit's or any
other), so `digitalio`, `busio.SPI`, `busio.I2C` and the libraries of the bundle built on them run
as on a microcontroller:

```
import board, busio, digitalio
import adafruit_bme280.basic as bme280
led = digitalio.DigitalInOut(board.D4)
led.switch_to_output(True)
sensor = bme280.Adafruit_BME280_I2C(board.I2C())
print(sensor.temperature, sensor.pressure, sensor.relative_humidity)
```

The names are Blinka's: `D0`-`D7` (ADBUS0-7), `C0`-`C7` (ACBUS0-7), `SCK`/`SCL` = `D0`,
`MOSI`/`SDA` = `D1`, `MISO` = `D2`; `board.SPI()` and `board.I2C()` give the shared buses.
`supervisor.runtime.display` is still the window. For I2C the chip reads SDA back on `D2`, so `D1` and `D2`
must be tied together (the switch on Adafruit's board does that). A chip-select for SPI is any
other pin as a `digitalio` output, as the bundle's drivers expect.

The program needs libusb-1.0 and the FT232H on a driver libusb can use:

- Linux: `libusb-1.0.so.0` (package `libusb-1.0-0`) and permission to the device, with a udev
  rule such as `SUBSYSTEM=="usb", ATTR{idVendor}=="0403", ATTR{idProduct}=="6014", MODE="0666"`
  in `/etc/udev/rules.d/99-ft232h.rules`. The `ftdi_sio` serial driver is detached automatically.
- Windows: `libusb-1.0.dll` beside `circuitpython.exe` (in the zip), and the FT232H switched
  from FTDI's driver to WinUSB with [Zadig](https://zadig.akeo.ie/): Options → List All Devices,
  pick "USB Serial Converter" with USB ID 0403 6014, choose WinUSB, Replace Driver. The chip stops
  being a COM port until the driver is rolled back in Device Manager.

What the chip's MPSSE engine cannot do: inputs have fixed pull-ups (`Pull.DOWN` raises), SPI
modes 1 and 3, I2C clock stretching, UART, PWM, analog and `neopixel`. Every pin access is a USB
round trip of about a millisecond, so bit-banging in Python is slow; SPI and I2C transfers move
whole buffers in one go. One FT232H at a time, the first one found; a USB error ends its use until
the program is restarted. `supervisor.reload()` puts every pin back to input,
`microcontroller.reset()` restarts the program and opens the chip again. Closing the window
releases the pins; a program killed from outside (Ctrl-\, a signal, the console
closed) leaves them as they were until the chip is opened again.

## Real pins with an MCP2221A

`--board mcp2221` does the same with an MCP2221A breakout, the plug-and-play one: it is a USB HID
device, so no driver change on Windows and no libusb anywhere. What it has: `G0`-`G3` for
`digitalio` (push-pull, no pull-ups: `Pull.UP`, `Pull.DOWN` and open drain raise), `SCL`/`SDA`
on the chip's own I2C engine (`busio.I2C`, `board.I2C()`, 47-400 kHz, clock stretching works),
`analogio.AnalogIn` on `G1`-`G3` (10 bits against VDD) and `analogio.AnalogOut` on `G2` or `G3`
(the one 5-bit DAC). What it lacks: SPI (`busio.SPI` raises), pull-ups, more than four pins; a pin
access takes about two milliseconds. `SCL` and `SDA` are bus pins only: `digitalio` on them
raises. The chip's settings are changed in SRAM only, so a power cycle restores its defaults.

Linux needs permission to the hidraw device, with a udev rule such as
`KERNEL=="hidraw*", ATTRS{idVendor}=="04d8", ATTRS{idProduct}=="00dd", MODE="0666"`. Windows needs
nothing. The chip's UART is a serial port of its own (a COM port, `/dev/ttyACM*`): `busio.UART`
on `board.TX`/`board.RX` uses it, found by the chip's USB IDs.

## Real pins on a Raspberry Pi: --board gpiochip

`--board gpiochip` runs on a Linux computer that has pins of its own, through the kernel's
interfaces and no library: the GPIO character device (`--gpiochip`, default `/dev/gpiochip0`),
an i2c-dev bus (`--i2c-bus`, default 1) and a spidev device (`--spi`, default
`/dev/spidev0.0`). The pins are named as Blinka names a Raspberry Pi's header: `D0`-`D27` (BCM
numbers, the chip's line offsets), `SDA` = `D2`, `SCL` = `D3`, `SCK`, `MOSI`, `MISO`, `CE0`,
`CE1`, and `board.I2C()`, `board.SPI()`; `board.TX`/`board.RX` are `/dev/serial0` unless `--uart`
names another port. `digitalio` has pull-ups, pull-downs and open drain where the chip offers
them; a pin the kernel uses (the I2C or SPI pins while their drivers are loaded) is "in use".
The I2C bus speed is the kernel's (`dtparam=i2c_arm_baudrate` on a Pi), not the program's, and
spidev drives `CE0` itself. The user needs access to the devices (the `gpio`, `i2c` and `spi`
groups on Raspberry Pi OS). Without a GPIO chip the board says so and does not start; on
Windows it is listed and refuses. Run on a Raspberry Pi with Raspberry Pi OS.

`neopixel` works on `MOSI` (`D10`) while spidev is there: each bit goes out as one SPI byte at
6.4 MHz, so the kernel's SPI driver keeps the timing. A write is at most as long as spidev's
buffer allows, `/sys/module/spidev/parameters/bufsiz` (4096 bytes by default, 170 pixels; raise
it with `spidev.bufsiz=` on the kernel's command line). Other pins raise, and `busio.SPI` cannot
share the pin with a strip. Checked on the Pi without LEDs attached; on a Pi 4 a changing core
clock can upset the timing (`core_freq_min=500` in `config.txt` holds it).

## A serial port as busio.UART

`--uart PORT` (`COM5`, `/dev/ttyUSB0`, a pseudo-terminal) puts `busio.UART(board.TX, board.RX,
...)` and `board.UART()` on that port of the computer, on any board; a USB serial adapter gives a
board's UART code real wires. Baud rate, 5-8 bits, parity, 1 or 2 stop bits, `timeout`,
`in_waiting`, `reset_input_buffer()` and a changed `baudrate` work; flow control and RS485
raise. On Linux only the standard baud rates. On the virtual board a UART takes any two free
pins, and without `--uart` its port is virtual: what the program writes goes to the bus trace
(`UART TX 4: 41 54 0d 0a  AT..`), a read waits its timeout and gets nothing. On other boards
without `--uart`, and not on an MCP2221, the class raises `RuntimeError`.

The virtual board's `TX`, `RX`, `SCL`, `SDA`, `SCK`, `MOSI` and `MISO` are plain pins too
(`digitalio`, `touchio` and the like take them), as on a microcontroller, while no bus holds them.

## The board panel: NeoPixels, buttons, sliders and other boards' pin names

On the virtual board the program's parts appear in a window, "CircuitPython board": NeoPixels
on top, then a row per part with its name, its control and its exact value. A part appears when
the program uses it, or ahead of the program from a board file.

| part | in the program | on the panel |
|---|---|---|
| NeoPixels | `neopixel_write` (the `neopixel` library) | a row or a matrix, as LEDs look |
| `[button]` | `digitalio` input | pressed while the mouse holds it (`"low"`, the default, or `"high"` when pressed) |
| `[switch]` | `digitalio` input | toggled by a click |
| `[led]` | `digitalio` output | a lamp in a colour (`"red"`, `"#ffa000"` ...) |
| `[analog]` | `analogio.AnalogIn` / `AnalogOut` | a slider (drag; the wheel moves 1/64, Shift+wheel 1 count) / a bar, raw and volts |
| `[pwm]` | `pwmio.PWMOut` | a lamp at the duty cycle, or with `"servo"` the arm's angle (750-2250 us for 0-180 degrees, as `adafruit_motor.servo` drives one); duty, frequency, pulse width |
| `[touch]` | `touchio.TouchIn` | a pad, touched while held |
| `[i2s]` | `audiobusio.I2SOut` | plays through the computer's sound output; a level meter, rate, bits and channels |
| `[mic]` | `audiobusio.PDMIn` | `"tone 440"` (the default) or `"noise"` as loud as a slider, or `"mic"`: the computer's microphone |
| `[encoder]` | `rotaryio.IncrementalEncoder` (on its first pin) | `-` and `+` turn it a detent, as does the wheel; the detents turned |
| `[ir]` | `pulseio.PulseIn` | a receiver: `send` hands it a remote's NEC code, the value (four bytes, `"255,2,247,8"` the Adafruit mini remote's 1, which an empty value means); a PulseIn on any virtual pin gets one |
| `[shiftreg]` | `keypad.ShiftRegisterKeys` (on its data pin) | the gamepad's buttons, shifted out in the order `keys=` gives: `"clock=PIN latch=PIN keys=B,A,START,SELECT"` |

`audiopwmio.PWMAudioOut` plays through the computer as `I2SOut` does, on the same speaker row.

`--board-file FILE` adds names to the chosen board, lays out NeoPixels and places parts, in TOML
as `settings.toml`:

```
[pins]
NEOPIXEL = "GPIO14"      # a pin number, 0-27
BUTTON = "SW_A"          # or a name the board has: here the Z key
A0 = "GPIO21"
[neopixel]
NEOPIXEL = "8x8 serpentine"
[button]
BUTTON = ""
[analog]
A0 = ""
```

NeoPixel layouts are `"10"` (a row), `"8x8"` (a matrix, filled row by row) or `"16x16
serpentine GRBW"` (every other row reversed, as most matrices are wired, and the byte order of
RGBW pixels; `GRB`, the WS2812's, is the default); `--neopixel NAME=LAYOUT` does the same on the
command line. On the virtual board pins 0-12 are the keyboard's buttons (pressed = low), 13 the
speaker and 14-27 free virtual pins. `boards/files/` has board files for a Feather, a Circuit
Playground, a Trinket or Gemma M0, a Pico (`GP0`-`GP27`, the Pico's own numbers, so 0-12 are
the keys too), a MagTag (its buttons the Z X A S keys), a CLUE (its five sensors on the virtual
I2C bus), a PyBadge and a PyGamer (their buttons the gamepad's, through a shift register), a
MatrixPortal (its HUB75 matrix on the panel) and the PicoPad's gamepad. A board file's `[board]` section names the real board where libraries
ask: `platform = "Atmel SAMD21"` is `sys.platform` (adafruit_circuitplayground picks by it),
`machine = "Adafruit Pybadge with samd51j19"` is `os.uname().machine` (adafruit_pybadger,
the MagTag and PyPortal libraries pick by it).

Every change goes to `--trace-bus`, the user's and the program's: `NEOPIXEL GPIO14 3: ff0000
00ff00 0000ff`, `BUTTON D9 pressed`, `GAMEPAD A pressed`, `PANEL A0 = 40000 (2.014 V)`, `PWM D5 duty 32768 (50.0%)
1000 Hz 500 us`, `TOUCH T1 on`, `LED D13 on`, `I2S I2S_DATA play 22050 Hz 16-bit mono`, `PWMAUDIO SPEAKER play ...`, `ENCODER ENC 3`, `IR IR_RX 255,2,247,8`, `PULSEOUT GPIO6 38000 Hz 67: 9000 4500 ...` (a
`pulseio.PulseOut`'s pulses in microseconds; `send()` takes as long as they last). Reads are not traced. The key script
(`CIRCUITPY_SDL_KEYS`) takes `set NAME VALUE`, `press NAME` and `release NAME` (a part, or a
gamepad button: `press START`) `turn NAME DETENTS` (an encoder, negative counter-clockwise) and `send NAME [CODE]` (an IR receiver: its
code, or `255,2,127,128`), so a test moves
the panel without a window. `pwmio`, `touchio` and `pulseio` exist on the virtual board only, `audiobusio` on every board (a record of
the generator takes as long as its samples last, as on a board); `analogio`
uses the MCP2221's ADC and DAC on `--board mcp2221`.

`usb_hid` and `usb_midi` are there on every board, with a board's devices and ports, so code
written for `adafruit_hid` and `adafruit_midi` runs; the port is no USB device, so what they send
goes to the trace only, `HID KEYBOARD 02 00 04 00 00 00 00 00`, `MIDI OUT 90 3c 64`, and nothing
comes in.

### Virtual I2C devices

On the virtual board `busio.I2C` is a virtual bus on any two pins, and `board.SCL`, `board.SDA`,
`board.I2C()` and `board.STEMMA_I2C()` reach it. A board file's `[i2c]` section puts device
models on it, `NAME = "MODEL [ADDRESS]"`; their libraries read them as the real parts, and each
value is a slider on the board panel (`ACCEL.x`, `ENV.temp` ...), set from the key script too
(`set ENV.temp 24.5`) and traced (`PANEL ENV.temp = 24.50 C`, beside the bus transfers):

| model | default address | values |
|---|---|---|
| `lis3dh` | 0x18 | x, y, z in g (lying still: z = 1) |
| `bme280` | 0x77 | temp (C), hum (%), press (hPa) |
| `mcp9808` | 0x18 | temp (C) |
| `ds3231` | 0x68 | the computer's local time; a time set moves it |
| `lc709203f` | 0x0b | percent, volt |
| `bmp280` | 0x77 | temp (C), press (hPa) |
| `lsm6ds33` | 0x6a | x, y, z in g (the gyro still) |
| `lis3mdl` | 0x1c | x, y, z in microtesla |
| `apds9960` | 0x39 | prox, red, green, blue (clear their sum; no gestures) |
| `sht31d` | 0x44 | temp (C), hum (%) |
| `crickit` | 0x49 | touch1-touch4 (raw, 300 untouched); a seesaw with the Crickit's pins: servos, motors and drives (PWM) and its NeoPixels go to the bus trace (`CRICKIT SERVO1 4915/65535 at 50 Hz: 1.50 ms high`), signals read their pull |

```
[i2c]
ACCEL = "lis3dh 0x19"
ENV = "bme280"
```

`boards/files/feather.toml` has all five, `circuitplayground.toml` the Circuit Playground's
LIS3DH for `adafruit_circuitplayground.express`. With no `[i2c]` the bus is empty, as a board
with nothing on its connector, until a driver looks for a device: an `I2CDevice` at an empty
address gets the first model above whose own address it is (0x6a an LSM6DS33, named LSM6DS33 on
the panel), as if the breakout were plugged in. A scan does not add any.

### Displays on the buses

`fourwire` and `i2cdisplaybus` drive displays as on a board, and on the virtual board they can be
virtual ones the board panel shows, upright as the module would be. A board file's `[spi]`
section puts colour TFTs on the virtual SPI bus (`board.SPI()`, `SCK`, `MOSI`, `MISO`, or any
pins), its `[i2c]` section SSD1306 OLEDs on the I2C one:

```
[spi]
TFT = "st7789 240x240 cs=D10 dc=D9"     # also ili9341, hx8357, gc9a01, st7735
[i2c]
OLED = "ssd1306 128x64"                  # 0x3c, or give the address: "ssd1306 128x32 0x3d"
```

The TFT model takes the display commands every one of these controllers shares (window,
memory write, display on and off) and learns where the module's glass sits in the controller's
memory from the first refresh, so a driver's `rowstart`/`colstart` come out right; the SSD1306
takes horizontal, vertical and page addressing. `adafruit_st7789`, `adafruit_ili9341`,
`adafruit_displayio_ssd1306` and the like run unchanged. The key script's `dump NAME PATH` saves
what a display shows as a PPM. The virtual board keeps its window (`supervisor.runtime.display`) beside them; a program
that calls `displayio.release_displays()` first releases it too, as on a board.
`supervisor.runtime.display.brightness` dims the window.

A display no board file names appears when the program makes it: a `busdisplay.BusDisplay` on a
`fourwire` bus with any cs pin gets a colour TFT (`TFT`, `TFT2`...) as large as the program says,
an `i2cdisplaybus` at any address an SSD1306 (`OLED`...), 128x32 when the driver says so. The
program's `rotation` is turned back, so the panel shows the picture upright, as on the module.

`rgbmatrix.RGBMatrix` on any of the virtual board's pins shows on the panel as the display `MATRIX` (HUB75
panels have no controller to model: what the program refreshes is what the panel shows, dark
while `brightness` is 0). `adafruit_matrixportal` builds it as on a MatrixPortal with
`boards/files/matrixportal.toml`. Tiled and serpentine chains show as the program lays them out.

### Wi-Fi

`wifi.radio` is the computer's network, so code written for a Wi-Fi board runs as it is:
`connect()` succeeds at once (the computer is online already; the network named goes to the
trace, `WIFI connect Home`), and `socketpool.SocketPool(wifi.radio)` opens the computer's
sockets, `adafruit_requests` and `adafruit_connection_manager` included. With
`CIRCUITPY_WIFI_SSID` in `settings.toml` the radio is connected when the program starts, as on
a board. `ipv4_address`, `ipv4_gateway`, `ipv4_subnet`, `ipv4_dns`, `mac_address` and
`hostname` are the computer's, from the interface its default route takes; a server a program
starts on `ipv4_address` is reachable from the computer and the network. When the computer will
not give a TCP server its port (another program has it, or it is below 1024 and the port runs on
Linux without root), the server takes 8080, 8081 ... (the ports after its own when that is 8080
or more) and the console says so: `port 80 is not allowed, using 8080`. UDP keeps its port.

`ap_info` and a scan give the network the computer is on: its Wi-Fi (`nmcli` on Linux, the WLAN
service on Windows) or, on Ethernet, one named as the program asked for. `ping()` is a real
ping. An access point, `wifi.Monitor`, a static address and setting the DNS raise
`NotImplementedError`. The web workflow keeps serving on the computer's network with no
`CIRCUITPY_WIFI_SSID`.

## Tracing the buses

`--trace-bus PATH` writes every I2C, SPI and UART transfer to PATH (`-` for stderr), one line
each, so `tail -f` shows what a library sends to a sensor:

```
    1.753 I2C 0x68 W 00 (no stop)
    1.756 I2C 0x68 R 3: 12 34 05
    2.010 I2C 0x55 probe -> NACK
    3.100 UART TX 6: 68 65 6c 6c 6f 0a  hello.
```

## Network

The host's network is a network interface like `wifi.radio`:

```
import hostnetwork, socketpool, ssl
pool = socketpool.SocketPool(hostnetwork.HostNetwork())
context = ssl.create_default_context()     # for adafruit_requests and other TLS code
```

TCP, UDP, DNS and TLS with the usual root certificates work. An accepted socket starts
non-blocking, as on the ESP32 port.

With `CIRCUITPY_WEB_API_PASSWORD` in `settings.toml` the web workflow serves the drive, the
serial console and the file editor at `http://127.0.0.1:8080` when
`CIRCUITPY_WEB_API_PORT = 8080` is set too (port 80 needs root). It listens on every interface,
as a board on Wi-Fi does, so other machines on the network reach it with the password.

## Debugging

`--debug [PORT]` loads the debug adapter (`debugadapter.py` beside the program) into the VM
before `code.py` and waits for an editor speaking the Debug Adapter Protocol on
`127.0.0.1:PORT` (default 5678). Then `code.py` runs under `sys.settrace`: breakpoints in
`code.py` and in `lib/*.py`, step into, over and out, the call stack, the local and global
variables of each frame, and expressions evaluated in a frame. The display keeps running in its
window while the program is stopped. `vscode/` is the VS Code extension: it starts the program
in a terminal and connects the editor (see `vscode/README.md`). Any other DAP client attaches
the same way.

A program under the debugger runs about three times slower. Python files compiled from source
show their locals; a `.mpy` keeps no variable names. After a reload (a file saved on the drive)
the program waits for the editor again.

## Controlling the program: --api

`--api [PORT]` opens a local HTTP interface for other programs (an editor, tests, an AI agent's
tools): the console as a stream and as input, code run in the REPL, the board panel's parts, the
displays as PNG and the bus trace. The default port is 8765; `--api 0` takes a free one. The
program says where on stderr:

```
circuitpython: api http://127.0.0.1:54321/ token 3f9c...
```

Every request carries `Authorization: Bearer TOKEN`. The token is `CIRCUITPY_API_TOKEN` when set
(a program that starts this one chooses it), else a random one. The interface listens on
127.0.0.1 only; `--api-host ADDR` listens elsewhere and needs `CIRCUITPY_API_TOKEN`. A request
naming another `Host`, or a browser's preflight, is refused, so a web page cannot reach it.
`exec` runs any code with the computer's files and network: the token is the key to that.

| endpoint | does |
|---|---|
| `GET /api/info` | version, board, drive, process id, `boot` (new after a reset), `status`: `code.py`, `REPL`, `Done` ...; `fast_time`, `skipped_ms` |
| `GET /api/console?since=N&wait=TEXT&timeout=MS` | the console's output from offset N; with `wait`, when TEXT has come (`matched`); `next` is where to read on |
| `POST /api/console` | the body, as typed (Ctrl-C included) |
| `POST /api/interrupt`, `POST /api/reload`, `POST /api/quit` | Ctrl-C; Ctrl-D (stopping code.py first); end the program |
| `POST /api/exec` | `{"code": ..., "timeout_ms": 10000, "interrupt": false}` → `{ok, output, value, error}` |
| `GET /api/state` | the panel's parts, NeoPixels, displays and gamepad buttons, as JSON |
| `POST /api/panel` | key script lines (`set ENV.temp 24.5`, `press A`, `turn ENC 2`, `send IR_RX`, `down Z`), applied before the answer |
| `GET /api/display?name=NAME` | the window (`DISPLAY`), or a board file's display, as PNG |
| `GET /api/trace?since=N&wait=TEXT&timeout=MS` | the bus trace, as the console; with or without `--trace-bus` |

`exec` types the code into the REPL as one line, so it runs as a person typing would: after
`code.py` has ended, in the REPL's variables, which last from one `exec` to the next. While
`code.py` runs it answers 409 unless `interrupt` is true. `value` is what the REPL would print
for an expression. A running program is the debugger's (`--debug`); a program that wants to be
watched prints, and the console stream has it. `state`, `panel` and `display` exist on the
virtual board. After `microcontroller.reset()` the program listens on the same port with the
same token.

`tools/cpapi.py` is a client with no dependencies beyond Python's own library:

```python
from cpapi import Board
with Board.launch("./circuitpython", "CIRCUITPY", board_file="feather.toml") as b:
    b.wait("ready")                     # the console, no sleeping
    b.press("BUTTON_A")
    b.wait("pressed")
    assert b.part("LED")["on"]
    b.set("ENV.temp", 29.5)
    print(b.eval("6 * 7", interrupt=True))   # '42'
    open("screen.png", "wb").write(b.display())
```

`tests/test_api.py` and the button and encoder cases of `tests/test_panel.py` use it.

### For AI agents: an MCP server

`tools/cp_mcp.py` (standard library only) serves the API to an agent over MCP's stdio transport.
Its tools launch the program on a drive (or attach to one already running), run code in the REPL,
read and wait for the console, type into it, interrupt and reload, read the state, move the panel,
return a display as an image and list, read and write the drive's files (a write reloads
`code.py`, as on a board). For Claude Code, in the project's `.mcp.json`:

```json
{"mcpServers": {"circuitpython": {"command": "python3",
  "args": ["ports/desktop/tools/cp_mcp.py", "--program", "ports/desktop/build-linux/circuitpython"]}}}
```

On Windows `python` and `build-windows/circuitpython.exe`. Without `--program` it takes
`CIRCUITPY_PROGRAM`, then the build next to `tools/`, then `circuitpython` on the PATH. One board at
a time; it runs headless unless `launch` asks for the window. `stop`, or the server's end, quits a
board it launched; one it attached to keeps running. Console text comes without escape sequences,
at most its last 20000 characters, and a call waits at most 60 s. `tests/test_mcp.py` drives it
as a host would.

### Fast time

`--fast-time` (`fast_time=True` in `cpapi`, `fast_time` in the MCP `launch`) is for tests of
programs that wait: a game whose enemy takes eight seconds to cross the screen, an animation
slowed down for the eye, a loop that reads a sensor every five seconds. A sleep returns at once
and moves the program's clock on by its length, so the program sees the time it expects and runs
as fast as the computer allows. The test waits for events (console text, a part's state, the
display), not for time; `GET /api/info` says `fast_time` and how much has been skipped.

What skips: `time.sleep`, `asyncio.sleep` and `select.poll` timeouts, every core delay,
`display.refresh(target_frames_per_second=...)`, in `code.py`, `boot.py` and the REPL alike.
`time.monotonic`, `supervisor.ticks_ms`, `time.time`, `rtc.RTC` and a board file's DS3231 all read
the program's clock. Sound is silent but plays by that clock, so `while audio.playing:` ends when
the sample would have; the virtual board's UART timeouts skip too. What does not skip: busy loops
on `time.monotonic()`, socket timeouts, the autoreload delay and the API's own timeouts. Input from the panel, the key script
(its `sleep MS` included) and the API arrives in the computer's time, so its moment in the game
is not exact. The window shows at most 60 pictures a second of the computer's time; the API's
display picture is always the latest. Output written faster than a test reads it can fall off the
console ring.

Real hardware runs in real time: `--fast-time` is refused on the `ft232h`, `mcp2221` and `gpiochip`
boards, with `--uart` and with the computer's microphone (`[mic]` `"mic"`).

## Without a screen

`SDL_VIDEODRIVER=dummy` runs without a display and `SDL_AUDIODRIVER=disk` writes the sound to a
file. `CIRCUITPY_SDL_KEYS` names a file or FIFO of lines `down KEY`, `up KEY` and `sleep MS`
(SDL key names such as `Z` or `Right`) that are played as key presses, so a test can drive a
program headless.

## Not yet

- Native `.mpy` files (turbo): loading them needs executable memory.
- On Windows: `audiomp3` (its decoder's inline assembly is for MSVC).
- mDNS and BLE (`_bleio`); an AirLift (ESP32 co-processor) for boards that reach the network
  through one.
- `pwmio`, `touchio`, `pulseio` and `rgbmatrix` on the virtual board only; `usb` (USB host),
  `alarm`, `sdcardio`, `picodvi` and `espcamera` not at all.
- Builds and packages for handhelds: the port needs a cross compiler with the device's SDL2
  (PortMaster for Anbernic and others, the OnionOS toolchain for the Miyoo Mini).
