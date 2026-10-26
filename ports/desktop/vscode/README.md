# CircuitPython host debugger for VS Code

Debug `code.py` running in the Linux or Windows build of CircuitPython: breakpoints, stepping,
the call stack, local and global variables, and expressions, with the display in its SDL window,
on the virtual board or with real pins through an FT232H or an MCP2221A.

The debug adapter runs inside CircuitPython (`debugadapter.py` beside the program, started by
`circuitpython --debug PORT`); this extension starts the program in a terminal and connects
VS Code to it.

## Install

Copy this folder to `~/.vscode/extensions/circuitpython-host-debug` (on Windows
`%USERPROFILE%\.vscode\extensions\`) and restart VS Code, or install the `.vsix`.

## Use

Open the CIRCUITPY folder in VS Code and add to `.vscode/launch.json`:

```json
{
  "type": "circuitpython",
  "request": "launch",
  "name": "CircuitPython: run code.py",
  "program": "/path/to/circuitpython",
  "drive": "${workspaceFolder}",
  "board": "ask",
  "port": 5678
}
```

Set breakpoints in `code.py` or in `lib/*.py` and press F5. The program starts in a terminal,
waits for the editor and then runs `code.py` under the debugger. Its console is the terminal.

## The board

`board` picks what the program runs as (`--board`):

| value | meaning |
|---|---|
| `ask` (default) | the board chosen in the status bar; asked on the first launch |
| `auto` | the one adapter that is plugged in; asked when none or several are |
| `virtual`, `ft232h`, `mcp2221`, `gpiochip` | that board, always |

The status bar shows the chosen board; clicking it, or **CircuitPython: Select board** in the
command palette, opens the list with each adapter marked as connected or not (the program's
`--list-boards`; set `circuitpython.program` to the program's path to see it before the first
launch). The choice is kept per workspace. A `--board` in `args` wins over `board`. A program
that is already running keeps its board until its terminal is closed.

`"args": ["--fast-time"]` runs the program in fast time: its sleeps return at once and move its
clock on, for a game or an animation that would take its time (virtual board only; see the
port's README).

## The board panel and other boards' pin names

`"boardFile": "${workspaceFolder}/feather.toml"` passes `--board-file`: names such as `NEOPIXEL`,
`D5`, `LED` added to the board, NeoPixel layouts and the parts of the board panel, the window
where buttons, switches, sliders, PWM lamps, touch pads and NeoPixels of the virtual board are
shown and moved (see the port's README; the port has `boards/files/feather.toml` and
`circuitplayground.toml`).

## The bus panel

With a board that has real pins, every I2C, SPI and UART transfer of the running program appears
in the **CircuitPython bus** output panel, one line each:

```
    1.753 I2C 0x68 W 00 (no stop)
    1.756 I2C 0x68 R 3: 12 34 05
    2.010 I2C 0x55 probe -> NACK
```

The panel opens by itself at the first line, without taking the focus; **CircuitPython: Show
bus trace** brings it back, and the panel's filter box narrows it to an address or a bus.
`"traceBus": false` in the launch configuration turns it off, `true` turns it on for the virtual
board too. It is the program's `--trace-bus FILE`, which works without the editor as well.

## Libraries

**CircuitPython: Install library…** asks for a name (`bme280`, `adafruit_bme280` or
`adafruit-circuitpython-bme280`) and puts that library and the libraries it needs into `lib/` on
the drive, from PyPI. **CircuitPython: Install libraries used by code.py** does the same for every
library `code.py` imports and names the ones PyPI does not have. They are installed as `.py`
source: the program runs it as well as `.mpy`, and the debugger shows local variables only for
source. Blinka and the packages only a computer needs are left out.

What may be installed comes from the index of the Adafruit CircuitPython Bundle (the file circup
reads), from its latest release on GitHub and checked against the sha256 GitHub lists: a library
found from an import, and every dependency, has to be in it, since anyone can publish a PyPI
project called `adafruit-circuitpython-something`. A name you type into Install library may be
any PyPI project. The files come from PyPI, each wheel checked against the sha256 PyPI lists.

`node libinstall.js DRIVE [--from-code] [NAME...]` does it without VS Code.

## Errors

`File "code.py", line 3` and `File "/lib/x.py", line 2` in a traceback in the CircuitPython
terminal are links to that line. An exception that ends `code.py` under the debugger also goes to
the **Problems** panel, on the line where it was raised; the next run clears it.

## Reconnecting

A saved file reloads the program, while `code.py` runs or after it ended, and the program waits
for the editor again; the extension notices and starts the next session by itself, so saving a
file and watching it stop at the breakpoint needs no F5. Stop in the editor never reconnects.
`circuitpython.reconnectAfterReload` turns this off.

To attach to a program you started yourself with `circuitpython --debug 5678`, use a
configuration with `"request": "attach"`.

Python files compiled from source show their local variables; `.mpy` files keep no variable
names, so for them only globals are visible.

## Tests

`node --test test/boards.test.js test/tail.test.js test/libinstall.test.js test/extension.test.js` runs
the board logic, the file follower, the library installer (its PyPI part skips offline) and the extension
against a fake `vscode` module.

`node test/e2e/run.js PATH_TO_CIRCUITPYTHON` runs it in a real VS Code with a fresh profile: a
breakpoint, a variable, the reconnect after a saved file, none after Stop. It uses the `code` on
the PATH, or `$VSCODE`; VS Code refuses to run tests while another instance of the same build is
open, so point `VSCODE` at a separate copy (an Insiders zip) to keep working in yours. With an
MCP2221 plugged in, `CP_E2E_MCP2221=1` adds an I2C scan that has to reach the bus panel.
