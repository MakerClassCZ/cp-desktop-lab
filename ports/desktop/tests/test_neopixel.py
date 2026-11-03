"""NeoPixels and a board file: python3 test_neopixel.py /path/to/circuitpython

--board-file adds pin names to the board and lays out NeoPixels; --neopixel does the latter on
the command line. What the program writes is checked in the bus trace, which needs no window.
"""

import os
import re
from harness import T, board_file, check, launch, refused, run

FEATHER = """# a few names of a Feather
[pins]
NEOPIXEL = "GPIO14"
LED = "GPIO15"
BUTTON = "GPIO4"    # an existing name
"D5" = "GPIO16"

[neopixel]
NEOPIXEL = "2x2 serpentine"
D5 = "3 RGB"
"""

WRITE = """import board, digitalio, neopixel_write
def show(pin, data):
    d = digitalio.DigitalInOut(pin)
    d.switch_to_output()
    neopixel_write.neopixel_write(d, bytes(data))
    d.deinit()
"""

out, trace = run(
    """import board, microcontroller
print("names", board.NEOPIXEL is microcontroller.pin.GPIO14, board.LED is microcontroller.pin.GPIO15,
      board.BUTTON is board.GPIO4, board.D5 is microcontroller.pin.GPIO16, "NEOPIXEL" in dir(board))
print("repr", repr(board.NEOPIXEL), repr(board.BUTTON))
""",
    "",
    "repr",
    FEATHER,
)
check("board file: new names and an alias", "names True True True True True" in out, out)
check(
    "board file: a pin's repr still works",
    "repr board.GPIO14 board.GPIO4" in out,
    out,
)

out, trace = run(
    WRITE
    + """show(board.NEOPIXEL, [255, 0, 0, 0, 255, 0, 0, 0, 255, 1, 2, 3])
show(board.D5, [255, 0, 0, 0, 255, 0, 0, 0, 255])
print("written")
""",
    "",
    "written",
    FEATHER,
)
lines = [l for l in trace.splitlines() if " NEOPIXEL " in l]
check(
    "neopixel_write: GRB bytes become RGB colours in the trace",
    len(lines) == 2 and lines[0].endswith("NEOPIXEL GPIO14 4: 00ff00 ff0000 0000ff 020103"),
    trace,
)
check(
    "neopixel_write: an RGB strip as the board file says",
    len(lines) == 2 and lines[1].endswith("NEOPIXEL GPIO16 3: ff0000 00ff00 0000ff"),
    trace,
)

out, trace = run(
    WRITE
    + """show(board.GPIO6, [0, 16, 0, 0, 0, 32, 64, 0, 0, 0, 0, 255])
print("written")
""",
    "",
    "written",
    args=["--neopixel", "GPIO6=3 GRBW"],
)
check(
    "--neopixel on the command line, GRBW as rrggbbww",
    "NEOPIXEL GPIO6 3: 10000000 20004000 000000ff" in trace,
    trace,
)

out, trace = run(
    WRITE
    + """show(board.GPIO7, [1, 2, 3, 4, 5, 6])
print("written")
""",
    "",
    "written",
)
check(
    "a pin nobody laid out: a row as long as the data",
    "NEOPIXEL GPIO7 2: 020103 050406" in trace,
    trace,
)

# [board] platform: sys.platform as the real board names it (adafruit_circuitplayground picks by it)
out, trace = run(
    'import os, sys\nprint("platform", sys.platform, sys.platform == "Atmel SAMD21")\n'
    'print("machine", "Pybadge" in os.uname().machine, os.uname().machine)\n',
    "",
    "machine",
    '[board]\nplatform = "Atmel SAMD21"\nmachine = "Adafruit Pybadge with samd51j19"\n',
)
check("[board] platform sets sys.platform", "platform Atmel SAMD21 True" in out, out)
check(
    "[board] machine sets os.uname().machine",
    "machine True Adafruit Pybadge with samd51j19" in out,
    out,
)

# A bad board file stops the start, whatever its section
for bad, why in (
    ('[pins]\nX = "NOPE"\n', "no pin NOPE"),
    ("[pins]\nX = GPIO99\n", "no pin GPIO99"),
    ("[colors]\n", "unknown section"),
    ('[neopixel]\nGPIO4 = "0x3"\n', "bad NeoPixel layout"),
    ('[board]\ncolour = "red"\n', "unknown [board] setting colour"),
    ('[board]\ndisplay = "240"\n', "[board] display 240: expected WIDTHxHEIGHT"),
    ('[button]\nD99 = ""\n', "no pin D99"),
    ('[led]\nGPIO4 = "plaid"\n', "bad colour"),
    ('[ir]\nGPIO4 = "1,2,3"\n', "bad IR code"),
    (
        '[spi]\nX = "st9999 240x240 cs=GPIO20 dc=GPIO21"\n',
        "unknown SPI display st9999 (st7789, ili9341, hx8357, gc9a01 or st7735)",
    ),
    ('[spi]\nX = "st7789 240x240"\n', "needs cs= and dc="),
    ('[gamepad]\nA = "keys=nokey"\n', "unknown key nokey"),
    ('[gamepad]\nZ = "z"\n', "no gamepad button Z"),
    (
        '[i2c]\nX = "bmp999"\n',
        "unknown I2C device bmp999 (lis3dh, bme280, bmp280, mcp9808",  # and the other models
    ),
):
    status, out = refused(board=bad)
    check(
        "a bad board file stops the start: " + why,
        status == 2 and why in out and "started" not in out,
        out,
    )

# The board files that come with the port load (beside the tests as board_files/, or in the tree).
files = next(
    (
        d
        for d in (os.path.join(T, "board_files"), os.path.join(T, "..", "boards", "files"))
        if os.path.isdir(d)
    ),
    None,
)
if files is None:
    print("SKIP no board files beside the tests")
else:
    for name in sorted(os.listdir(files)):
        # A file naming a NeoPixel or an LED has it on the board; every file loads.
        text = open(os.path.join(files, name)).read()
        src = 'import board\nprint("loaded", %s, %s)\n' % (
            "board.NEOPIXEL is not None" if "NEOPIXEL =" in text else "True",
            "board.LED is not None" if "\nLED =" in text else "True",
        )
        # A file with a display size has the display that size.
        size = re.search(r'^display = "(\d+)x(\d+)"', open(os.path.join(files, name)).read(), re.M)
        if size:
            src = 'import board\nprint("size", board.DISPLAY.width, board.DISPLAY.height)\n' + src
        out, trace = run(src, "", "loaded", args=["--board-file", os.path.join(files, name)])
        check("board file " + name, "loaded True True" in out, out)
        if size:
            check(
                "board file %s: a %sx%s display" % (name, size[1], size[2]),
                "size %s %s" % (size[1], size[2]) in out,
                out,
            )
# Several board files, read in order: a later one uses an earlier one's names, and its part wins
first = '[pins]\nMYBTN = "GPIO5"\n[led]\nGPIO6 = "red"\n'
second = board_file('[led]\nGPIO6 = "green"\n[pins]\nALIAS = "MYBTN"\n', name="second.toml")
out, _ = run(
    'import board\nprint("names", board.MYBTN is board.ALIAS)\n',
    board=first,
    args=("--board-file", second),
    expect="names",
)
check("two board files: the second uses the first's names", "names True" in out, out)
with launch('print("up")\n', board=first, args=("--board-file", second)) as b:
    b.wait("up")
    leds = [p for p in b.state()["parts"] if p["name"] == "GPIO6"]
    check(
        "two board files: the later file's part wins",
        [p["colour"] for p in leds] == ["20e020"],
        leds,
    )

print("DONE")
