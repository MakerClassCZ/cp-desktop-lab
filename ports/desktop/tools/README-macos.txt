CircuitPython for macOS: an experimental, unofficial build of ports/desktop

The program is not signed by Apple. Once, after unpacking, clear the download mark in Terminal,
or macOS refuses to open it:

    xattr -dr com.apple.quarantine circuitpython-macos-arm64

Run it from Terminal, in this folder:

    ./circuitpython                        CIRCUITPY is the folder ./CIRCUITPY, created on first start
    ./circuitpython --drive ~/path/to/folder

A board's names, NeoPixels and parts: boards/files/ holds board files of real boards (pybadge.toml,
clue.toml, feather.toml, magtag.toml, ...), so code written for one runs with its pin names:

    ./circuitpython --board-file boards/files/pybadge.toml --drive ~/path/to/project
    ./circuitpython --drive examples/panel/CIRCUITPY --board-file examples/panel/board.toml

Parts from Python: on the virtual board board.parts lists what can be plugged in, board.attach
and board.detach plug parts in and out from code.py or the REPL, and board.attached is what is
on the board (board.attached["BME280"].temp = 30); examples/parts shows a button, an LED and a
sensor:

    ./circuitpython --drive examples/parts/CIRCUITPY

A MIDI keyboard and CC sliders on the board panel, which usb_midi.ports[0] reads as from a USB
MIDI keyboard (play it with the mouse or the computer keys Z X C ... and Q W E ...); examples/synth
is a synthio synth with a filter and an echo on three sliders:

    ./circuitpython --drive examples/synth/CIRCUITPY --board-file examples/synth/board.toml

README.md is the whole manual: the board panel, virtual sensors and displays, --fast-time,
--trace-bus, the web workflow, the network, the MCP server.

The terminal is the serial console: Ctrl-C interrupts, Ctrl-D reloads, any key opens the REPL,
Ctrl-\ or closing the window quits. Edit the files in the folder with any editor; code.py runs
again after each save. A window opens for the display; keys: arrows, Z X A S (A B X Y),
Enter (START), Backspace (SELECT), Q W (L R), Esc (MENU); F12 saves a screenshot.

DEBUGGING IN VS CODE
1. Install the extension: in VS Code, Extensions > "..." > Install from VSIX >
   circuitpython-host-debug-*.vsix (from the release page).
2. Open your project folder, the one with code.py (File > Open Folder), and add to
   .vscode/launch.json a configuration of type "circuitpython" whose "program" is the path of
   this folder's circuitpython.
3. Set a breakpoint in code.py and press F5. debugadapter.py must stay beside circuitpython.

REAL PINS WITH AN FT232H
    ./circuitpython --board ft232h --drive ~/path/to/project
gives board.D0-D7, C0-C7, busio.SPI and busio.I2C through an FT232H breakout on USB.
libusb-1.0.0.dylib beside the program does the USB (its license is in libusb-LICENSE.txt).

REAL PINS WITH AN MCP2221A
    ./circuitpython --board mcp2221 --drive ~/path/to/project
gives board.G0-G3, busio.I2C, analogio and the chip's serial port as board.UART(); no driver
is needed.

SDL2.framework must stay beside the program: it draws the window and plays the sound.

A crash prints "hard fault, backtrace:": send those lines back with what you ran.
