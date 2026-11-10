"""Touchscreens: python3 test_touch.py /path/to/circuitpython

A touch on a display (the panel line "touch NAME X Y", as the mouse makes it) is read by what the
board file binds to that display: a resistive panel on four pins, through adafruit_touchscreen's
algorithm (inlined: the library is not in the repository), or an I2C touch controller (FT6206,
CST816, TSC2007) through its registers.
"""

import os
from harness import T, check, launch, refused, seen

PYPORTAL = next(
    p
    for p in (
        os.path.join(T, "board_files", "pyportal.toml"),
        os.path.join(T, "..", "boards", "files", "pyportal.toml"),
    )
    if os.path.exists(p)
)

# adafruit_touchscreen 1.x's touch_point, its pins and calibration as the PyPortal's examples.
RESISTIVE = """import board, time
from analogio import AnalogIn
from digitalio import DigitalInOut

XL, XR, YD, YU = board.TOUCH_XL, board.TOUCH_XR, board.TOUCH_YD, board.TOUCH_YU
CAL = ((5200, 59000), (5800, 57000))
SIZE = (board.DISPLAY.width, board.DISPLAY.height)

def map_range(x, a, b, c, d):
    v = (x - a) * (d - c) / (b - a) + c
    return max(min(v, max(c, d)), min(c, d))

def touch_point():
    with DigitalInOut(XR) as x_p:
        x_p.switch_to_output(False)
        with DigitalInOut(YD) as y_m:
            y_m.switch_to_output(True)
            with AnalogIn(XL) as x_m:
                z_1 = x_m.value
            with AnalogIn(YU) as y_p:
                z_2 = y_p.value
    z = 65535 - (z_2 - z_1)
    if z <= 10000:
        return None
    with DigitalInOut(YU) as y_p, DigitalInOut(YD) as y_m, AnalogIn(XR) as x_p:
        y_p.switch_to_output(True)
        y_m.switch_to_output(False)
        x = sum(x_p.value for _ in range(4)) / 4
    x = int(map_range(x, CAL[0][0], CAL[0][1], 0, SIZE[0] - 1))
    with DigitalInOut(XR) as x_p, DigitalInOut(XL) as x_m, AnalogIn(YU) as y_p:
        x_p.switch_to_output(True)
        x_m.switch_to_output(False)
        y = sum(y_p.value for _ in range(4)) / 4
    y = int(map_range(y, CAL[1][0], CAL[1][1], 0, SIZE[1] - 1))
    return (x, y, z)

last = "?"
print("ready")
while True:
    p = touch_point()
    if repr(p) != last:
        last = repr(p)
        print("TP", None if p is None else p[:2], "pressed" if p and p[2] > 10000 else "")
    time.sleep(0.01)
"""

with launch(RESISTIVE, open(PYPORTAL).read()) as b:
    seen(b, "ready")
    check("resistive: untouched reads None", seen(b, "TP None"), b.console(since=0)["data"])
    results = {}
    for x, y in ((0, 0), (319, 239), (100, 50), (250, 180)):
        b.panel("touch DISPLAY %d %d" % (x, y))
        seen(b, "pressed")
        line = b.console(since=0)["data"].rsplit("TP (", 1)[-1]
        rx, ry = (int(v) for v in line.split(")")[0].split(","))
        # The library truncates its mapped value: a pixel short on either axis is the same touch.
        results[(x, y)] = (rx, ry) if abs(rx - x) > 1 or abs(ry - y) > 1 else True
    check(
        "resistive: the corners and points in between read where they were touched",
        all(r is True for r in results.values()),
        results,
    )
    state = [d for d in b.state()["displays"] if d["name"] == "DISPLAY"][0]
    check(
        "resistive: the panel's state has the touch",
        state.get("touch") == {"down": True, "x": 250, "y": 180},
        state,
    )
    b.panel("untouch DISPLAY")
    check("resistive: lifted, None again", seen(b, "TP None"), b.console(since=0)["data"][-300:])
    trace = b.trace(since=0)["data"]
    check(
        "resistive: the touches in the trace",
        "TOUCHSCREEN DISPLAY 100,50" in trace and "TOUCHSCREEN DISPLAY up" in trace,
        trace[-400:],
    )
    check(
        "resistive: no slider rows for the panel's pins",
        not any(p["name"].startswith("TOUCH_") for p in b.state()["parts"]),
        [p["name"] for p in b.state()["parts"]],
    )

# A capacitive controller's registers, read as adafruit_focaltouch and adafruit_cst8xx read them.
REGS = """import board, time
i2c = board.I2C()
while not i2c.try_lock():
    pass
def read(addr, reg, n):
    buf = bytearray(n)
    i2c.writeto_then_readfrom(addr, bytes([reg]), buf)
    return buf
ADDR = %s
print("ids", read(ADDR, %s, %d).hex())
print("ready")
last = None
while True:
    d = read(ADDR, 0, 16)
    n = d[2]
    s = "touches %%d" %% n
    if n:
        x = (d[3] & 0x0F) << 8 | d[4]
        y = (d[5] & 0x0F) << 8 | d[6]
        s += " at %%d,%%d event %%d" %% (x, y, d[3] >> 6)
    if s != last:
        print(s)
        last = s
    time.sleep(0.01)
"""

# An FT6206 on a 240x320 TFT the program turns to 320x240: it reports in the glass's frame.
TFT = """import board, displayio, fourwire, busdisplay
displayio.release_displays()
bus = fourwire.FourWire(board.SPI(), command=board.D9, chip_select=board.D10)
INIT = b"\\x01\\x80\\x96\\x11\\x80\\xff\\x3a\\x81\\x55\\x0a\\x36\\x01\\x08\\x29\\x80\\xff"
display = busdisplay.BusDisplay(bus, INIT, width=320, height=240, rotation=90)
"""
FT_BOARD = '[pins]\nD9 = "GPIO18"\nD10 = "GPIO19"\n[spi]\nTFT = "st7789 240x320 cs=D10 dc=D9"\n[i2c]\nCTP = "ft6206"\n'
with launch(TFT + REGS % ("0x38", "0xa1", 8), FT_BOARD) as b:
    seen(b, "ready")
    out = b.console(since=0)["data"]
    check(
        "ft6206: library version, chip 0x06, vendor 0x11",
        "ids 3003060000100011" in out,
        out[-300:],
    )
    check("ft6206: untouched, no touches", seen(b, "touches 0"), out[-300:])
    b.panel("touch TFT 300 10")
    check(
        "ft6206: a touch on the turned display at the glass's 229,300, pressed first",
        seen(b, "touches 1 at 229,300 event 0") and seen(b, "touches 1 at 229,300 event 2"),
        b.console(since=0)["data"][-300:],
    )
    b.panel("touch TFT 20 200")
    check("ft6206: dragged on", seen(b, "touches 1 at 39,20"), b.console(since=0)["data"][-300:])
    b.panel("untouch TFT")
    check("ft6206: lifted", seen(b, "touches 0"), b.console(since=0)["data"][-300:])
    sliders = {
        p["name"]: p.get("value") for p in b.state()["parts"] if p["name"].startswith("CTP")
    }
    check(
        "ft6206: its channels on the panel",
        "CTP.x" in sliders and "CTP.touched" in sliders,
        sliders,
    )

# A CST816 on the main window, with the panel's own swap.
with launch(REGS % ("0x15", "0xa6", 6), '[i2c]\nCTP = "cst816 swap"\n') as b:
    seen(b, "ready")
    out = b.console(since=0)["data"]
    check("cst816: chip 0xb5 at 0xa7", "ids 01b50101b5" in out, out[-300:])
    b.panel("touch DISPLAY 10 20")
    check(
        "cst816: swapped, 20,10", seen(b, "touches 1 at 20,10"), b.console(since=0)["data"][-300:]
    )

# A TSC2007 follows a touch on the main window: raw 0-4095 over it, the pressure while down.
TSC = """import board, time
i2c = board.I2C()
while not i2c.try_lock():
    pass
def read(cmd):
    buf = bytearray(2)
    i2c.writeto_then_readfrom(0x48, bytes([cmd]), buf)
    return (buf[0] << 4) | (buf[1] >> 4)
print("ready")
last = None
while True:
    s = "tsc %d %d %d" % (read(0xC4), read(0xD4), read(0xE4))
    if s != last:
        print(s)
        last = s
    time.sleep(0.01)
"""
with launch(TSC, '[i2c]\nTS = "tsc2007"\n') as b:
    seen(b, "ready")
    w = [d for d in b.state()["displays"] if d["name"] == "DISPLAY"][0]["width"]
    b.panel("touch DISPLAY %d 0" % (w - 1))
    check(
        "tsc2007: the right edge reads 4095, pressed",
        seen(b, "tsc 4095 0 1000"),
        b.console(since=0)["data"][-300:],
    )
    b.panel("untouch DISPLAY")
    check(
        "tsc2007: lifted, no pressure", seen(b, "tsc 4095 0 0"), b.console(since=0)["data"][-300:]
    )

# What is refused
status, said = refused(board='[touchscreen]\nT = "resistive GPIO0 GPIO1"\n')
check("a resistive panel needs four pins", "a [touchscreen] is" in said, said)
status, said = refused(
    board='[touchscreen]\nT = "resistive GPIO0 GPIO1 GPIO2 GPIO3 calibration=1,1,2,3"\n'
)
check("a calibration needs two different readings per axis", "calibration=" in said, said)
with launch('print("ready")\nwhile True: pass\n') as b:
    seen(b, "ready")
    try:
        b.panel("touch DISPLAY 1 2")
        ok = False
    except Exception as e:
        ok = "no touchscreen" in str(e)
    check("a touch where nothing reads it is refused", ok)
