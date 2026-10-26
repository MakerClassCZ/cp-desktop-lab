# Tests of the port

`harness.py` holds what the tests share: `check()` and its PASS/FAIL line, a fresh drive, the
program started through the control API (`tools/cpapi.py`, found beside the tests or in
`../tools`) and driven by panel commands that each wait for the program's answer, the display's
picture, a start that should fail, `Run`, the program as a process of its own for what the
API does not show (a drive's first start, a reload, a reset), and `Chip`, the runs of the two
USB-adapter tests.

Each script takes the program and works in its own directory, so run them from a scratch copy:

    cp -r ports/desktop/tests /tmp/cplt && cp ports/desktop/tools/cpapi.py ports/desktop/tools/cp_mcp.py /tmp/cplt && cd /tmp/cplt
    B=/path/to/build-linux/circuitpython          # or build-windows/circuitpython.exe on Windows
    for t in test_*.py; do python3 $t $B; done
    FT232H_LOOP=D4,D5 FT232H_SPI_LOOP=1 FT232H_BME280=0x76 python3 test_ft232h.py $B   # with an FT232H
    MCP2221_LOOP=G2,G3 MCP2221_UART_LOOP=1 MCP2221_PCF8523=0x68 python3 test_mcp2221.py $B   # with an MCP2221A

- `test_folder.py`: the directory drive (defaults, auto-reload on host edits, read-only Python
  and `storage.remount`, `os` functions and their errors, imports, `storage.map_file`,
  settings.toml, the web workflow, reset).
- `test_image.py`: the FAT image drive (`DEMO_IMAGE=path.img` also boots an existing image).
- `test_erase.py`: `storage.erase_filesystem()` leaves a directory alone.
- `test_dap.py`: the debugger over the Debug Adapter Protocol, like an editor: breakpoints,
  stack, scopes, locals, evaluate, step into a library file, step out, step over, Stop, the
  reconnect after a saved file, and the report of an exception that ended `code.py`.
- `test_board_option.py`: `--board`: an unknown name, the virtual board's names, `busio` refusing
  to construct without an FT232H.
- `test_ft232h.py`: real pins through an FT232H; skips without one. Tiers by what is wired:
  always (the board, inputs, outputs, `Pull.DOWN`, reloads and resets, a second program refused),
  `FT232H_LOOP=D4,D5` (a wire between two pins), `FT232H_SPI_LOOP=1` (D1 tied to D2, as the
  board's I2C switch does), `FT232H_BME280=0x76` (a BME280 on SCL/SDA, read with
  `adafruit_bme280` fetched by pip into `libcache/` beside the script).
- `test_mcp2221.py`: real pins through an MCP2221A; skips without one. Always: the board, `digitalio`
  refusing the bus pins, pulls and open drain, outputs, reloads and resets, a second program
  refused, SPI refused, an absent I2C address, a clean second run after `analogio`;
  `MCP2221_LOOP=G2,G3` (digital loopback, `AnalogIn` following the driven pin, `AnalogOut` read
  back); `MCP2221_UART_LOOP=1` (TX wired to RX: `busio.UART` and `board.UART()` on the chip's own
  serial port); `MCP2221_PCF8523=0x68` (a PCF8523 clock on the chip's bus, e.g. on STEMMA QT:
  registers, long reads and writes, `adafruit_pcf8523`). Both chip tests check `--trace-bus` too.
- `test_uart.py`: `busio.UART` on `--uart` with a socat pseudo-terminal pair (Linux; skips
  without socat): both directions and their trace, a timeout, `in_waiting`, the baud rate, the
  refusals.

`tools/survey_learning_guides.py` is not a test: it runs Learning Guide programs and counts how
many work on the port.

The scripts were written on Linux; on Windows they run with Python for Windows against the
`.exe`, minus the Linux-only memory-map checks. A reset on
Windows starts a child process and waits for it, so a crash at start-up once nested 947
processes and the machine ran out of memory: run a new build with a process limit or a watchful
eye, and when killing leftovers never match the program by a substring that the test's own
command line contains.
