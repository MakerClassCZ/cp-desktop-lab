# The desktop port for agents: reproducing and checking bugs in the core

CircuitPython's shared code (`shared-module/`, `shared-bindings/`, `py/`, `supervisor/`) runs
here as a program on Linux, Windows or macOS, with a virtual board, displays, buses and parts.
This file is for an agent that uses it to reproduce an issue, find its cause and check a fix
before a board does. `CLAUDE.md` beside it is for working on the port itself; `README.md` has
the details of everything below.

## The program

- From a release (MakerClassCZ/cp-desktop-lab): `circuitpython-linux-x86_64`, `-aarch64`,
  `-macos-arm64`, `-windows-x64`; `circuitpython-linux-i386` is 32-bit, as the MCUs are; each
  has a `-debug` twin with the same program unstripped.
- From source, to test a change in the shared code: `make -C ports/desktop` (Linux, macOS),
  the Windows cross build in README.md. `make CC="gcc -m32" BUILD=build-linux-i386` for 32-bit.
- The port follows adafruit/circuitpython `main` with a delay. A fix meant for a PR goes on a
  branch from upstream `main`; check it there on hardware too.

## One run from the command line

```
python3 ports/desktop/tools/cpapi.py run PROGRAM repro.py --wait drawn --colors --png shot.png
```

`repro.py` (or a directory, as the whole drive) runs once, headless, until `code.py` ends or
`--wait`'s text comes; the console is printed without the terminal's escapes, then the display's
pixels per colour and the PNG if asked. `--runs N` runs it N times in one process (Ctrl-D), for
bugs of a second run; `--board-file`, `--display NAME`, `--fast-time`, `--timeout S`. Exit status
0, 1 when it did not end in time, 2 after a hard fault, whose backtrace is printed last.

## Driving it: tools/cpapi.py

To drive the program while it runs (buttons, sensors, the REPL), in Python. Standard library only. Headless by default, no sleeps needed:

```python
import sys; sys.path.insert(0, "ports/desktop/tools")
from cpapi import Board
with Board.launch("ports/desktop/build-linux/circuitpython", "/tmp/repro/CIRCUITPY") as b:
    b.wait("done")                          # code.py's output, waited for
    print(b.console(since=0)["data"])       # all of it
    p = b.pixels()                          # board.DISPLAY's pixels
    print(p.count_colors(), p.diff(lambda x, y: 0xFFFFFF if y == 10 else None)[:5])
    b.reload()                              # Ctrl-D: code.py again, as after a save
    print(b.eval("6 * 7", interrupt=True))  # the REPL
```

- `code.py` (and `lib/`) go in the drive directory before the launch; a file written while it
  runs reloads it, as on a board.
- `board_file="x.toml"` adds pins, NeoPixels, parts and virtual I2C/SPI devices and displays
  (`[i2c] OLED = "ssd1306 128x64"`); see README.md for the models.
- `b.pixels(name)`: a display's pixels (`width`, `height`, `rgb`, `p[x, y]` as 0xRRGGBB),
  `count_colors()`, `diff(expected)` against another picture or a function of (x, y).
- `b.trace()`/`b.trace_wait(text)`: every I2C, SPI and UART transfer.
- `b.state()`, `b.panel("set ENV.temp 24.5", "press A")`: parts, buttons, sensors.
- `fast_time=True`: sleeps return at once and move the clock on, for code that waits long.
- `b.ble`: the phone of a Bluetooth LE program (`BLERadio()`, `_bleio`). `scan()` is what it
  advertises (`name`, `services`); `p = b.ble.connect()`, then `p.services()`, `p.read(uuid)`,
  `p.write(uuid, data, response=True)`, `p.subscribe(uuid)` and `p.notifications(uuid)`,
  `p.uart_write(text)` and `p.uart_read()` for the Nordic UART, `p.disconnect()`;
  `cpapi.bluefruit.color(r, g, b)` (and `button`, `accelerometer`) are Bluefruit Connect's
  packets. The trace has `BLE ADV`, `BLE CONNECT`, `BLE ATT <-`/`->` and `BLE DISCONNECT`.
- `tools/cp_mcp.py` serves the same to an agent over MCP (`ble_scan`, `ble_connect`, ...).

## Good evidence for an issue or a PR

- Before and after, the same script: the console, the pixels that differ (`diff()`), the number
  of runs that fail (`reload()` in a loop for bugs of a second run).
- A crash prints `hard fault`, the command for its backtrace (`addr2line -f -e circuitpython`,
  `atos -o circuitpython` on macOS) and the program's addresses; give them to that command with
  the unstripped program (a `-debug` package or the build's) for functions and lines. Lines
  marked `outside the program` are libc's or a DLL's.
- Say where each result came from ("desktop lab", the board) and what was not tested.

## What it does not show

- Real timing and real peripherals: the buses and displays are models, read and written as a
  board's driver would, not the chips.
- Faults a Cortex-M has and a PC has not (an unaligned access, a stack that overflows in a few
  KB). A wild pointer often crashes here too, but not always.
- A board's memory: the heap is 16 MB unless `--ram MB` (`args=["--ram", "1"]`) says less.
- 64-bit sizes: use the i386 program when `size_t`, pointers or `mp_int_t` may matter.
- The nRF port's own `_bleio`: BLE here is the HCI host stack the AirLift boards use
  (`devices/ble_hci`), as a peripheral only (no scanning or connecting from `code.py`, no
  pairing), at an MTU of 23. A bug of a CLUE's SoftDevice code needs the board. The lab's
  adapter comes on at its first use, so a program that sets `adapter.enabled = False` and then
  uses it finds it on again, where a CLUE raises.

## Tests

`sh ports/desktop/tools/ci-test.sh PROGRAM [test_x.py ...]` runs the port's tests
(`ports/desktop/tests/`) as CI does.
