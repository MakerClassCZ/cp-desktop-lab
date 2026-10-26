CircuitPython for Windows: an experimental, unofficial build of ports/desktop

Run it from a terminal (Windows Terminal, cmd or PowerShell):

    circuitpython.exe                      CIRCUITPY is the folder .\CIRCUITPY, created on first start
    circuitpython.exe --drive demo\picostellar
    circuitpython.exe --drive C:\path\to\folder

The terminal is the serial console: Ctrl-C interrupts, Ctrl-D reloads, any key opens the REPL,
Ctrl-\ or closing the window quits. Edit the files in the folder with any editor; code.py runs
again after each save. A window opens for the display; keys: arrows, Z X A S (A B X Y),
Enter (START), Backspace (SELECT), Q W (L R), Esc (MENU); F12 saves a screenshot. Game
controllers work through XInput.

DEBUGGING IN VS CODE
1. Install the extension: in VS Code, Extensions > "..." > Install from VSIX >
   circuitpython-host-debug-*.vsix.
2. Open your project folder, the one with code.py (File > Open Folder), and add to
   .vscode\launch.json a configuration of type "circuitpython" whose "program" is the path of
   circuitpython.exe (README of the extension: Extensions > CircuitPython host debugger).
3. Set a breakpoint in code.py or lib\helper.py and press F5. The program starts in a terminal
   inside VS Code, waits for the editor, then runs code.py under the debugger: stepping, the
   call stack, Locals and Globals, and the Watch and Debug Console evaluate expressions.
   After you save a file the program reloads and waits again; press F5 again.
   debugadapter.py must stay beside circuitpython.exe.

REAL PINS WITH AN FT232H
    circuitpython.exe --board ft232h --drive C:\path\to\project
gives board.D0-D7, C0-C7, busio.SPI (SCK D0, MOSI D1, MISO D2) and busio.I2C (SCL D0, SDA D1,
with D1 and D2 tied) through an FT232H breakout on USB; libraries of the bundle run unchanged.
The chip has to be on the WinUSB driver: in Zadig (zadig.akeo.ie), Options > List All Devices,
pick "USB Serial Converter" with USB ID 0403 6014, choose WinUSB, Replace Driver. libusb-1.0.dll
must stay beside circuitpython.exe (its license is in libusb-LICENSE.txt).

REAL PINS WITH AN MCP2221A
    circuitpython.exe --board mcp2221 --drive C:\path\to\project
gives board.G0-G3, busio.I2C, analogio and the chip's serial port as board.UART(); no driver
is needed.

Not on Windows yet: audiomp3. Sound plays through the default audio device.

A crash prints "hard fault, exception ... at 0x...": send that line back with what you ran.
