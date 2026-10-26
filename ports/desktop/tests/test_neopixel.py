"""NeoPixels and a board file: python3 test_neopixel.py /path/to/circuitpython

--board-file adds pin names to the board and lays out NeoPixels; --neopixel does the latter on
the command line. What the program writes is checked in the bus trace, which needs no window.
"""
import os
from harness import T, check, refused, run

FEATHER = '''# a few names of a Feather
[pins]
NEOPIXEL = "GPIO14"
LED = "GPIO15"
BUTTON = "SW_A"     # an existing name: the Z key
"D5" = "GPIO16"

[neopixel]
NEOPIXEL = "2x2 serpentine"
D5 = "3 RGB"
'''

WRITE = '''import board, digitalio, neopixel_write
def show(pin, data):
    d = digitalio.DigitalInOut(pin)
    d.switch_to_output()
    neopixel_write.neopixel_write(d, bytes(data))
    d.deinit()
'''

out, trace = run('''import board, microcontroller
print("names", board.NEOPIXEL is microcontroller.pin.GPIO14, board.LED is microcontroller.pin.GPIO15,
      board.BUTTON is board.SW_A, board.D5 is microcontroller.pin.GPIO16, "NEOPIXEL" in dir(board))
print("repr", repr(board.NEOPIXEL), repr(board.BUTTON))
''', "", "repr", FEATHER)
check("board file: new names and an alias", "names True True True True True" in out, out)
check("board file: a pin's repr still works", "repr microcontroller.pin.GPIO14 board.SW_A" in out, out)

out, trace = run(WRITE + '''show(board.NEOPIXEL, [255, 0, 0, 0, 255, 0, 0, 0, 255, 1, 2, 3])
show(board.D5, [255, 0, 0, 0, 255, 0, 0, 0, 255])
print("written")
''', "", "written", FEATHER)
lines = [l for l in trace.splitlines() if " NEOPIXEL " in l]
check("neopixel_write: GRB bytes become RGB colours in the trace",
      len(lines) == 2 and lines[0].endswith("NEOPIXEL GPIO14 4: 00ff00 ff0000 0000ff 020103"), trace)
check("neopixel_write: an RGB strip as the board file says", len(lines) == 2 and lines[1].endswith("NEOPIXEL GPIO16 3: ff0000 00ff00 0000ff"), trace)

out, trace = run(WRITE + '''show(board.SW_X, [0, 16, 0, 0, 0, 32, 64, 0, 0, 0, 0, 255])
print("written")
''', "", "written", args=["--neopixel", "SW_X=3 GRBW"])
check("--neopixel on the command line, GRBW as rrggbbww", "NEOPIXEL GPIO6 3: 10000000 20004000 000000ff" in trace, trace)

out, trace = run(WRITE + '''show(board.SW_Y, [1, 2, 3, 4, 5, 6])
print("written")
''', "", "written")
check("a pin nobody laid out: a row as long as the data", "NEOPIXEL GPIO7 2: 020103 050406" in trace, trace)

# [board] platform: sys.platform as the real board names it (adafruit_circuitplayground picks by it)
out, trace = run('import os, sys\nprint("platform", sys.platform, sys.platform == "Atmel SAMD21")\n'
                 'print("machine", "Pybadge" in os.uname().machine, os.uname().machine)\n', "", "machine",
                 '[board]\nplatform = "Atmel SAMD21"\nmachine = "Adafruit Pybadge with samd51j19"\n')
check("[board] platform sets sys.platform", "platform Atmel SAMD21 True" in out, out)
check("[board] machine sets os.uname().machine", "machine True Adafruit Pybadge with samd51j19" in out, out)

# A bad board file stops the start, whatever its section
for bad, why in (('[pins]\nX = "NOPE"\n', "no pin NOPE"), ('[pins]\nX = GPIO99\n', "no pin GPIO99"),
                 ('[colors]\n', "unknown section"), ('[neopixel]\nSW_A = "0x3"\n', "bad NeoPixel layout"),
                 ('[board]\ncolour = "red"\n', "unknown [board] setting colour"),
                 ('[button]\nD99 = ""\n', "no pin D99"), ('[led]\nSW_A = "plaid"\n', "bad colour"),
                 ('[ir]\nSW_A = "1,2,3"\n', "bad IR code"),
                 ('[spi]\nX = "st9999 240x240 cs=GPIO20 dc=GPIO21"\n', "unknown SPI display st9999 (st7789, ili9341, hx8357, gc9a01 or st7735)"),
                 ('[spi]\nX = "st7789 240x240"\n', "needs cs= and dc="),
                 ('[gamepad]\nA = "nokey"\n', "unknown key nokey"), ('[gamepad]\nZ = "z"\n', "no gamepad button Z"),
                 ('[i2c]\nX = "bmp999"\n', "unknown I2C device bmp999 (lis3dh, bme280, bmp280, mcp9808, ds3231, lc709203f, lsm6ds33, lis3mdl, apds9960, sht31d, crickit or ssd1306)"),
                 ('[i2c]\nA = "lis3dh 0x18"\nB = "mcp9808"\n', "address 0x18 taken")):
    status, out = refused(board=bad)
    check("a bad board file stops the start: " + why, status == 2 and why in out and "started" not in out, out)

# The board files that come with the port load (beside the tests as board_files/, or in the tree).
files = next((d for d in (os.path.join(T, "board_files"), os.path.join(T, "..", "boards", "files")) if os.path.isdir(d)), None)
if files is None:
    print("SKIP no board files beside the tests")
else:
    for name in sorted(os.listdir(files)):
        # A file naming a NeoPixel and an LED has them on the board; one that only shows the gamepad loads.
        names = "NEOPIXEL =" in open(os.path.join(files, name)).read()
        src = 'import board\nprint("loaded", board.NEOPIXEL is not None, board.LED is not None)\n' if names else 'print("loaded", True, True)\n'
        out, trace = run(src, "", "loaded", args=["--board-file", os.path.join(files, name)])
        check("board file " + name, "loaded True True" in out, out)
print("DONE")
