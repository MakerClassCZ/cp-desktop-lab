# The desktop port for agents: reproducing and checking bugs in the core

CircuitPython's shared code (`shared-module/`, `shared-bindings/`, `py/`, `supervisor/`) runs
here as a program on Linux, Windows or macOS, with a virtual board, displays, buses and parts.
This file is for an agent that uses it to reproduce an issue, find its cause and check a fix
before a board does. `CLAUDE.md` beside it is for working on the port itself; `README.md` has
the details of everything below.

## The program

- From a release (MakerClassCZ/cp-desktop-lab): `circuitpython-linux-x86_64`, `-aarch64`,
  `-macos-arm64`, `-windows-x64`; `circuitpython-linux-i386` is 32-bit, as the MCUs are; each
  has a `-debug` twin with the same program unstripped. In a package, `cpapi.py`, `cp_mcp.py`
  and this file lie beside the program: below, `ports/desktop/tools/` and the build directories
  are that folder.
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
bugs of a second run; `--code FILE` (a file of a drive directory in place of `code.py`),
`--board-file`, `--display NAME`, `--fast-time`, `--timeout S`. Exit status
0; 1 when it did not end in time or the program ended; 2 after a hard fault, whose backtrace is
printed last; 3 when `code.py` ended in an exception; 4 when it reported a fail with
`lab.result`; 64 for wrong arguments.

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
- An `EPaperDisplay` gets a modelled panel, `b.pixels("EPD")`, shown only at a refresh, with
  its busy pin; `b.state()` counts its `refreshes`. A board file's `epaper cs=PIN refresh_ms=N`
  or `stuck` changes how long it is busy.
- `b.state()`, `b.panel("set ENV.temp 24.5", "press A")`: parts, buttons, sensors.
- In the program, `import lab` (none on a board): `lab.attach(lab.parts.LED(board.GPIO5))`,
  `lab.panel("press A")`, and `lab.result("fail", detail)` for a test that says itself whether
  it passed; `b.result()`, `cpapi.py run` (exit 4) and `ab` read it. `board` has only what a
  board has.
- Faults, for the bugs of what goes wrong: `lab.panel("fault sd.write error after=3 once")` in
  the program or `b.panel(...)` from outside; points `sd.read`, `sd.write` (`error`, `timeout`),
  `i2c` (`nack`, `hold`, `addr=`); `set SD present 0`, `set EPD busy stuck`. `lab.fired()`,
  `b.fired()`, `faults_fired` in `b.after_run()`, a row in `ab`. README.md, Faults.
- A repro to attach: `code.py` plus a playbook, `cpapi.py ab MAIN FIX code.py --playbook
  x.txt`: `wait "TEXT"`, a fault or a press, `expect "TEXT"`; it plays on the program's clock
  at each run and its outcome is the run's result. README.md, The playbook.
- `fast_time=True`: sleeps return at once and move the clock on, for code that waits long.
- `b.after_run()` once `code.py` has ended: `leaked_pins`, the pins objects left claimed (as
  on the boards' ports, a run frees nothing in bulk: the next run gets "in use"), and
  `outside_heap_change`, memory outside the heap gained since the run before; with `reload()` in
  a loop for the bugs of resources that outlive their VM.
- `audio_capture=True`, then `b.audio()`: a `Recording` per audio output with `peak()`, `rms()`,
  `frequency()`, `silences()`, `underruns` and `save("x.wav")`; `audiocore.get_buffer()` is there
  as in the unix coverage build. `cpapi.py run ... --wav x.wav` does it from the command line.
- `code_file="games/snake.py"`: a file of the drive run in place of `code.py`, every run.
- `b.ble`: the phone of a Bluetooth LE program (`BLERadio()`, `_bleio`). `scan()` is what it
  advertises (`name`, `services`); `p = b.ble.connect()`, then `p.services()`, `p.read(uuid)`,
  `p.write(uuid, data, response=True)`, `p.subscribe(uuid)` and `p.notifications(uuid)`,
  `p.uart_write(text)` and `p.uart_read()` for the Nordic UART, `p.disconnect()`;
  `cpapi.bluefruit.color(r, g, b)` (and `button`, `accelerometer`) are Bluefruit Connect's
  packets. The trace has `BLE ADV`, `BLE CONNECT`, `BLE ATT <-`/`->` and `BLE DISCONNECT`.
- A program that scans and connects (the central): `with cpapi.Air(b) as air:`, then
  `hrm = air.add(cpapi.devices.heart_rate())`, `hrm.beat(80)`; `cpapi.devices.uart()` with
  `send(text)` and `received(timeout)`; `cpapi.devices.beacon(data)`; `cpapi.Peripheral(name,
  [cpapi.GattService(uuid, [cpapi.GattCharacteristic(uuid, ("read", "notify"), value)])])` for
  any other, with `notify()`, `written(uuid)`, `wait_connected()`, `disconnect()`.
  `cpapi.Air(a, b)` puts two programs on one air (CLUE to CLUE). The trace has `BLE SCAN`,
  `BLE REPORT` and `BLE CONNECT ... central`.
- `tools/cp_mcp.py` serves the same to an agent over MCP (`ble_scan`, `ble_connect`, ...;
  `ble_device_add`, `ble_device_notify`, `ble_device_written`, `ble_device_remove` for a central;
  `ble_pair`, `ble_bonds`, `ble_erase_bonds` for pairing).

## Good evidence for an issue or a PR

- `cpapi.py ab MAIN FIX repro.py --runs 5` prints the before-and-after table; `tools/build-variant.sh
  NAME COMMIT|RANGE|FILE.diff` builds the lab with the fix (HEAD of this checkout plus it).
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
- A board's memory: the heap may grow to 16 MB; `--profile rp2040` (`samd51`, `esp32s3`,
  `rp2350`; `args=["--profile", "rp2040"]`) stops it where that board's stops, so
  `MemoryError` comes where it would there; small objects still take more room here.
- 64-bit sizes: use the i386 program when `size_t`, pointers or `mp_int_t` may matter.
- The nRF port's own `_bleio`: BLE here is the HCI host stack the AirLift boards use
  (`devices/ble_hci`), as a peripheral and as a central, at an MTU of 23; it pairs (LE Legacy,
  Just Works) and bonds as a peripheral only, not as a central (`pair()` there raises).
  Its devices and other labs answer at once: radio timing, lost packets and a distance are
  not there. A bug of a CLUE's SoftDevice code needs the board. The lab's
  adapter comes on at its first use, so a program that sets `adapter.enabled = False` and then
  uses it finds it on again, where a CLUE raises.

## Tests

`sh ports/desktop/tools/ci-test.sh PROGRAM [test_x.py ...]` runs the port's tests
(`ports/desktop/tests/`) as CI does.
