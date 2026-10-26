# Developing the port

Notes for whoever continues this port on another machine. The user-facing documentation is
`README.md`; this file is where things are and what bit us.

## Branch and layout

Branch `linux-port` on top of adafruit/circuitpython `main` (246a7abe44). Everything of the port
is under `ports/desktop/`; the shared code carries small `CIRCUITPY-CHANGE` edits for the host
(clang, MinGW, `supervisor_vfs` host directories, `sys.settrace` with `frame.f_locals`, the
`port_before_user_code()` hook). Commits carry no Co-Authored-By or similar trailers.

The sources are grouped: `host/` is the computer's side that is not hardware (window, panel,
control API, trace, debugger hook, network), `hardware/` real pins through adapters, `virtual/`
the simulated board (panel parts, NeoPixels, displays, the virtual I2C and SPI devices). The root
keeps the process, the runtime board table and what the shared build expects there.
`platform/posix/` and `platform/windows/` hold what differs between the systems; the Makefile
builds one of them (`PLATFORM`) and puts it on the include path, so `host_socket.h` and the
functions of `platform/platform.h` are each system's own.

| where | what |
|---|---|
| `supervisor/`, `desktop_main.c`, `interrupts.c` | the process: options, stack, the timer thread that plays the interrupt controller, console, drive image, reset, crash handling |
| `platform/platform.h`, `platform/*/{console,process,drive}.c` | the system's side of those: the terminal, signals or console events, faults, the reset, the drive's mapping |
| `desktop_watch.c`, `platform/*/watch.c` | reload on host edits of a directory drive (inotify, ReadDirectoryChangesW) |
| `platform/*/host_socket.h`, `common-hal/socketpool`, `bindings/hostnetwork` | the host's sockets with one face (BSD, Winsock) |
| `host/sdl_host.c`, `bindings/sdl`, `common-hal/sdl`, `common-hal/audioio` | display, buttons and sound through SDL2 |
| `desktop_pins.c/.h`, `desktop_board.c`, `boards/desktop/*pins.c` | the pin backend struct (virtual pins behind it), `--board` picking a board table and a backend at start-up |
| `common-hal/busio`, `host/bus_trace.c` | `busio` dispatching to the backend's I2C and SPI ops (board.I2C()/SPI() pass placeholder pins it turns into the board's), `--trace-bus` |
| `hardware/usb_host.c`, `hardware/mpsse*.c` | the FT232H: libusb-1.0 loaded at run time, the MPSSE engine and the GPIO shadow, SPI and the bit-banged I2C behind the backend's ops |
| `platform/*/hid_host.c`, `hardware/mcp2221.c`, `common-hal/analogio` | the MCP2221A: the system's HID (hid.dll, hidraw), GPIO, the chip's I2C engine behind the same ops, ADC and DAC |
| `hardware/gpiochip.c`, `boards/desktop/gpiochip_pins.c` | `--board gpiochip`: the kernel's GPIO character device, i2c-dev and spidev (run on a Raspberry Pi 4: GPIO, I2C with a BME688 and an SSD1306, SPI opened) |
| `host/panel.c`, `virtual/parts.c`, `virtual/neopixel_emu.c`, `common-hal/{neopixel_write,pwmio,touchio,analogio}` | the board panel: virtual parts on the virtual board's pins (buttons, switches, LEDs, sliders, PWM, touch, NeoPixels) in one SDL window, the terminal font for text |
| `virtual/i2c_virtual.c`, `virtual/i2c/`, `virtual/spi_virtual.c`, `virtual/vdisplay.c`, `virtual/display_auto.c` | the virtual buses and their device models, one file each in `virtual/i2c/` (`[i2c]`, `[spi]` in a board file); `make CIRCUITPY_VIRTUAL_DEVICES=0` leaves the models out (build it in its own `BUILD=` directory: make does not notice a changed setting and would keep the old objects) |
| `host/control_api.c`, `host/api_*.c`, `tools/cpapi.py`, `tools/cp_mcp.py` | `--api`: the control API, its client and the MCP server |
| `board_file.c`, `boards/files/` | `--board-file`/`--neopixel`: names added to the board at start-up (strings outside the heap, so they outlive each VM, and qstrs in each VM for Tab completion), NeoPixel layouts, parts |
| `platform/*/serial_host.c`, `common-hal/busio/UART.c` | `busio.UART` on a serial port of the computer (`--uart`, the MCP2221's own) |
| `host/debug_hook.c`, `python/debugadapter.py`, `vscode/` | the debugger: `--debug PORT`, the adapter inside the VM, the editor extension (board choice, bus panel, Problems, library installer; `vscode/test/e2e` runs it in a real VS Code) |
| `tests/` | the test scripts (see `tests/README.md`) |
| `tools/bootstrap.sh` | submodules and the Windows toolchain on a fresh machine |
| `tools/package-windows.sh` | the zip for Windows users |

The shared changes outside the port, in case of a rebase: `py/circuitpy_mpconfig.h` (alloca on
Windows, clang passes the GCC version check, `CIRCUITPY_FILESYSTEM_POSIX`), `py/circuitpy_defns.mk`
untouched but its GCC-only flags are filtered in the Makefile for clang, `supervisor/filesystem.h`
and `supervisor/shared/filesystem.c` (the host-directory variant beside FAT and littlefs),
`supervisor/shared/port.c` (TLSF defaults only with TLSF, `port_before_user_code`), `main.c`
(the hook), `shared-module/storage/__init__.c` (`map_file` by mmap or MapViewOfFile),
`shared-module/terminalio/Terminal.c` (no nested function), `supervisor/shared/web_workflow/web_workflow.c`
(no `strchrnul`), `extmod/vfs_posix*.c/.h` (public struct with a blockdev prefix, static init,
file descriptor accessor, no VfsPosix stdio), `py/emitbc.c`, `py/emitglue.h`, `py/profile.c`,
`py/objcell.c` (local names for `f_locals`), `shared-bindings/microcontroller/Pin.c` (a pin's
repr skips board names that are strings, not qstrs), `lib/tjpgd/src/tjpgd.h` (`_MSC_VER`).

## Build

`tools/bootstrap.sh` fetches the submodules and prints the exact commands. Linux: `make`
(gcc >= 10, libsdl2-dev). Windows: a cross build with llvm-mingw, see README. After changing
`mpconfigport.h` or `mpconfigport.mk`, delete the build directory: the generated qstr and module
tables are not rebuilt on a configuration change. clang warns about more than GCC; the Makefile
filters the GCC-only flags and silences the classes the GCC builds keep checking.

A native build on Windows: `tools/bootstrap-msys2.sh` in an MSYS2 UCRT64 shell installs GCC,
SDL2, Python and make, then `make BUILD=build-windows SDL_DIR=/ucrt64`. The Makefile takes
`OS=Windows_NT` as a Windows build and GCC keeps its own flags (the clang filters do not apply).
This path has not been tried; expect some friction in the shared Makefiles. An agent running
in Git Bash can run the MSYS2 shell for the build:
`C:/msys64/msys2_shell.cmd -ucrt64 -defterm -no-start -here -c "make ..."`.

Windows is tested on the user's PC: the same `tests/` with Python for Windows against the `.exe`.

macOS has no build yet. `platform/posix/` is written for Linux where macOS differs, so a macOS
build needs its own: the drive watcher (inotify; FSEvents or kqueue), HID (hidraw; IOKit), the
reset and the program's directory (`/proc/self/exe`; `_NSGetExecutablePath`), the Wi-Fi name
(nmcli), and `sys.platform` (`darwin`, `boards/desktop/mpconfigboard.h` has the branch). The
GPIO character device is Linux's and stays refused elsewhere; `tools/cpapi.py`'s parent-death
signal and the tests' `/proc` checks are Linux-only too.

## Things that bit

- **A reset on Windows is a new process the old one waits for.** A crash at start-up restarted
  forever, 947 nested processes exhausted the memory and the OOM killer took the whole session
  down, twice. Now a second hard fault right after a restart ends the program and Windows stops
  nesting after sixteen resets. Still: watch the process count with a new build and kill
  leftovers by PID, never by a substring the test's own command line contains.
- The VM's filesystem is the drive: `sys.path` cannot reach the host, so the debug adapter is
  read from beside the program in C and compiled into a module.
- A local variable changed after `nlr_push()` is stale after an exception (`setjmp` semantics):
  free such buffers on the success path only.
- MicroPython has no `del bytearray[slice]`; `bytearray.extend(memoryview)` works.
- `supervisor_tick()` is for scheduling only; SDL and audio work runs once per tick from
  `port_background_tick()`, never from `port_background_task()` (every VM hook, 30x slower).
- Python sees a directory drive read-only unless `boot.py` remounts it, as a board with USB does;
  `main.c` hands the drive to Python when there is no USB, so the directory variant forces the
  "host writable" flag.
- The debugger adds Python frames above every call of the user's: the host has a 256 KB
  pystack, a board's 2 KB ended picostellar early.
- **The FT232H puts two status bytes at the start of every USB packet** (512 bytes), not of
  every transfer: a reader strips them per 512-byte stretch of the buffer it got.
- **The chip holds about two packets towards the host and stalls the engine on a third**, and
  with synchronous libusb the host only polls the IN endpoint while a read is pending. So a
  command stream whose replies would make three packets never finishes sending. Two rules keep
  it under: the latency timer is at its longest (255 ms) so replies form full 512-byte packets
  rather than one packet per millisecond, and every stream ends with `0x87` (send immediate) so
  nothing waits for the timer. Then a flush may expect up to ~1000 reply bytes: full-duplex SPI
  goes in 512-byte pieces, I2C reads in batches of 389; write-only and read-only commands may be
  64 KiB (a read-only command's replies are polled as soon as its 3-byte header is sent).
  Measured with a pyusb probe: 900 replies per stream pass, 1100 stall; with the 1 ms timer
  already 100 stalled.
- After a crash mid-transfer the engine keeps streaming into the next process: `mpsse_open()`
  drains until the chip is empty before the sync check.
- On Windows the parent closes the device before `CreateProcessA` on a reset, or the child cannot
  open it; `desktop_power_off()` never touches libusb (it runs on the signal thread or the console
  control handler while the VM thread may be inside a transfer), and neither does the fault path.
- Timeouts scale with the bus clock (a second plus the bytes' time, plus what earlier sends left
  queued): the chip takes bytes only as its engine clocks the earlier ones out, so a write at
  2 kHz followed by one at 20 kHz stalled the second past a timeout computed from its own clock. A receive that
  times out drains the chip first, or the late bytes become the next call's data.
- The packet size comes from the descriptor: 512 at high speed, 64 behind a USB 1.1 hub or in a
  VM, and the status bytes land every packet.
- Backend entries never raise: `reset_port()` runs outside any `nlr_push`. A lost device is a flag
  the next Python call turns into `OSError`.
- The MPSSE engine drives SK and DO only when the direction byte says output; claiming a pin
  configures nothing. Under drive-only-zero (`0x9E`) an output "1" is a released line, which is
  how I2C works, with D1 and D2 tied on the board.
- **pywinusb answers wrongly for every MCP2221 command but 0x10**; the hidapi extension module
  (`hid.cp313-win_amd64.pyd`, shadowed by the ctypes `hid` package of the same name) is the
  reliable probe. The C transport talks to hid.dll directly: a report is 64 bytes, written with a
  leading report id 0 (hidraw strips it), one exchange about 2 ms.
- The MCP2221's SRAM remembers GP designations (ADC, DAC, LED functions) across runs but not
  power cycles; `open` sets every GP back to a GPIO input, and a crashed run's I2C transfer is
  cancelled with the status command before anything else.
- A HID device another program holds exclusively cannot be asked for its attributes, so the
  Windows transport matches `vid_xxxx&pid_xxxx` in the interface path and then reports the
  exclusive open's failure as "cannot open".
- A trailing comment on a Makefile variable (`CIRCUITPY_X = 1   # why`) puts spaces into the
  value and `ifeq ($(CIRCUITPY_X),1)` silently fails: the module simply is not there.
- Building on this machine: WSL from `/mnt/c` (slow, 3-6 min a build), llvm-mingw extracted in
  the WSL home (its symlinks do not survive the NTFS mount), the upstream tags fetched for
  `mpversion.h`, `huffman` installed by hand, and GCC 11 needs `-Wno-error=maybe-uninitialized`
  for `shared-module/lvfontio`. The Linux build meets the chip only after `usbipd attach` from an
  admin prompt on Windows, which takes it from Windows; the Windows program is the hardware gate
  (`tests/test_ft232h.py`).

## The FT232H and the next transports

`--board ft232h`, `--board mcp2221` and `--board gpiochip` are done (designs and plans in
`docs/superpowers/`): `desktop_pin_backend_t` is the seam. A backend owns open/close, a bank-wide
reset, per-pin configure/write/read, a lost flag, which pins `digitalio` may take, and optional
I2C and SPI ops tables (`desktop_i2c_ops_t`, `desktop_spi_ops_t`, each naming its pins) that
`common-hal/busio` dispatches to; `desktop_board.c` pairs a board table with a backend. `analogio`
is still written against the MCP2221 and would get the same ops treatment with a second
provider.

Candidates next: a Pico running U2IF (the same HID style as the MCP2221, with SPI and PWM). With
the libusb transport in
place, CP's `usb.core` (shared-bindings/usb/core) can get a host implementation almost for free.
FTDI's D2XX was the alternative transport on Windows; libusb with WinUSB made it unnecessary.
Selecting among several FT232H (by bus address; the user's chip has a blank EEPROM, so not by
serial) and recovering from a USB error without a restart are the open ends on the FT232H
itself.
