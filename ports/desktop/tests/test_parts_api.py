"""Parts plugged in from Python: python3 test_parts_api.py /path/to/circuitpython

board.parts is the catalogue, board.attach puts a part on the board, and an attached part's
values are its attributes, the same state the panel shows.
"""

from harness import check, launch

PINS = '[pins]\nD5 = "GPIO16"\nD6 = "GPIO17"\nD7 = "GPIO18"\n'

with launch('print("up")\n', board=PINS) as b:
    b.wait("up")
    names = b.exec("import board\nprint(sorted(dir(board.parts)))")
    check(
        "catalogue: pin parts, sensors, strips; no display",
        all(n in names for n in ("'BME280'", "'Button'", "'LED'", "'NeoPixel'", "'Potentiometer'"))
        and "SSD1306" not in names,
        names,
    )
    out = b.exec(
        'led = board.parts.LED(board.D5, color="green")\nprint(repr(led))\nboard.attach(led)\nprint(repr(led))'
    )
    lines = out.splitlines()
    check(
        "an LED: described, then attached",
        len(lines) == 2 and "not attached" in lines[0] and lines[1] == "<LED D5 on D5: off>",
        out,
    )
    out = b.exec(
        "import digitalio\nd = digitalio.DigitalInOut(board.D5)\nd.switch_to_output(True)\nprint(led.value)"
    )
    check("an LED's value is what the program drives", out.strip() == "True", out)
    out = b.exec(
        'env = board.parts.BME280(address=0x76, name="ENV")\nboard.attach(env)\nenv.temp = 35\n'
        "i2c = board.I2C()\nprint(i2c.try_lock() and i2c.scan())\ni2c.unlock()\nprint(env.temp)"
    )
    check("a BME280 at 0x76 answers and holds the value set", "118" in out and "35.0" in out, out)
    parts = b.state()["parts"]
    check(
        "the panel shows the set value",
        any(p["name"] == "ENV.temp" and abs(p["value"] - 35) < 0.01 for p in parts),
        parts,
    )
    out = b.exec(
        "e2 = board.parts.BME280(address=0x76, name='ENV')\nboard.attach(e2)\nprint(e2.temp)"
    )
    check("the same part attached again keeps its values", "35.0" in out, out)
    out = b.exec("print(repr(env))")
    check(
        "a sensor's repr shows its place and values",
        out.startswith("<BME280 ENV on I2C at 0x76: temp=35.00 C"),
        out,
    )
    out = b.exec(
        'try:\n    board.parts.LED(board.D6).value\nexcept RuntimeError as e:\n    print("err", e)'
    )
    check("a value of a part not attached raises", "D6 is not attached" in out, out)
    out = b.exec("env.temp = 1000\nprint(env.temp)")
    check("an out-of-range value is clamped as the slider is", "1000" not in out, out)
    out = b.exec(
        "pot = board.attach(board.parts.Potentiometer(board.D6))\npot.value = 70000\nprint(pot.value)\n"
        "import analogio\nprint(analogio.AnalogIn(board.D6).value)"
    )
    check(
        "a potentiometer: clamped, and what AnalogIn reads", out.split() == ["65535", "65535"], out
    )
    out = b.exec(
        'try:\n    board.attach(board.parts.MCP9808(name="TOOLONGNAME12"))\nexcept ValueError as e:\n    print("err", e)'
    )
    check("a name over the limit is refused, not cut", "err" in out and "11" in out, out)
    out = b.exec(
        'try:\n    board.attach(board.parts.Button(board.SCL))\nexcept ValueError as e:\n    print("err", e)'
    )
    check("a bus's pin is refused", "err SCL" in out, out)
    out = b.exec(
        'try:\n    board.attach(board.parts.LED(board.D6, color="plaid"))\nexcept ValueError as e:\n    print("err", e)'
    )
    check("a bad option says what the board file would", "err bad colour" in out, out)
    out = b.exec(
        "sw = board.attach(board.parts.Switch(board.SW_A))\nsw.on = True\n"
        "print(sw.on, digitalio.DigitalInOut(board.SW_A).value)"
    )
    check("a switch set from Python drives its pin", out.strip() == "True True", out)
    out = b.exec(
        'np = board.attach(board.parts.NeoPixel(board.D7, layout="2 GRBW"))\nprint(len(np.colors), np.colors[0])'
    )
    check("a NeoPixel strip's colours", out.strip() == "2 (0, 0, 0, 0)", out)
    out = b.exec("print(repr(board.parts.BME280))")
    check(
        "a kind's repr lists its keywords and channels",
        "address=0x77" in out and "temp C" in out,
        out,
    )
    b.write("board.parts.BM\t")
    check("Tab completes the catalogue", "BME280" in b.wait("BME280", timeout=5), "")

print("DONE")
