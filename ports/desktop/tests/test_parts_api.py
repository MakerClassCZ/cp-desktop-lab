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

# Detach: the place behaves as with the wire pulled; last wins at a place; slots are reused.
PINS3 = PINS + 'A0 = "GPIO19"\nD9 = "GPIO20"\n[led]\nD9 = "red"\n'
with launch('print("up")\n', board=PINS3) as b:
    b.wait("up")
    b.exec(
        'import board, digitalio, analogio\nenv = board.parts.BME280(address=0x76, name="ENV")\n'
        "board.attach(env)\nenv.temp = 30\ni2c = board.I2C()"
    )
    out = b.exec(
        'board.detach("ENV")\ni2c.try_lock()\ntry:\n    i2c.writeto(0x76, bytes([0xd0]))\n'
        'except OSError as e:\n    print("err", e.errno)\ni2c.unlock()\nprint(repr(env))'
    )
    check("a detached I2C device does not answer", "err 19" in out, out)
    check("a detached part says so", "<BME280 ENV on I2C at 0x76, not attached>" in out, out)
    check(
        "the panel leaves a detached device out",
        not any(p["name"].startswith("ENV.") for p in b.state()["parts"]),
        b.state()["parts"],
    )
    out = b.exec("board.attach(env)\nprint(env.temp)")
    check("attached again, a device keeps its values", out.strip() == "30.0", out)
    out = b.exec(
        "from adafruit_bus_device.i2c_device import I2CDevice\ntry:\n    I2CDevice(i2c, 0x77)\n"
        "    I2CDevice(i2c, 0x77)\nexcept ValueError as e:\n    print('err', e)"
    )
    check("the automatic attaching fills an untouched address", "err" not in out, out)
    b.exec('board.detach("BME280")')
    out = b.exec("try:\n    I2CDevice(i2c, 0x77)\nexcept ValueError as e:\n    print('err', e)")
    check("an emptied address stays empty", "err" in out, out)
    out = b.exec('try:\n    board.detach("NOPE")\nexcept ValueError as e:\n    print("err", e)')
    check("detaching an unknown name raises", "err" in out and "NOPE" in out, out)

    b.exec(
        "pot = board.parts.Potentiometer(board.A0)\nboard.attach(pot)\npot.value = 40000\n"
        "ain = analogio.AnalogIn(board.A0)"
    )
    out = b.exec("board.detach(pot)\nprint(ain.value)")
    check("an AnalogIn of a pulled potentiometer reads nothing", out.strip() == "0", out)
    out = b.exec("board.attach(pot)\npot.value = 50000\nprint(ain.value)")
    check("an AnalogIn made before the part was pulled works when it is back", "50000" in out, out)

    out = b.exec(
        "btn = board.attach(board.parts.Button(board.D7))\nbtn.pressed = True\n"
        "d7 = digitalio.DigitalInOut(board.D7)\nd7.pull = digitalio.Pull.UP\nprint(d7.value)\n"
        "board.detach(btn)\nprint(d7.value)"
    )
    check("a pulled button's pin reads its pull", out.split() == ["False", "True"], out)
    b.exec("d7.deinit()")

    b.exec("board.attach(board.parts.LED(board.D5))\nboard.attach(board.parts.Switch(board.D5))")
    kinds = [p["kind"] for p in b.state()["parts"] if p.get("pin") == "GPIO16"]
    check("last wins at a pin", kinds == ["switch"], b.state()["parts"])
    b.exec('board.attach(board.parts.Touch(board.D6, name="D5"))')
    names = [
        (p["name"], p["kind"]) for p in b.state()["parts"] if p.get("pin") in ("GPIO16", "GPIO17")
    ]
    check("last wins by name", names == [("D5", "touch")], names)
    out = b.exec(
        "for i in range(100):\n    l = board.parts.LED(board.D7)\n    board.attach(l)\n    board.detach(l)\nprint('done')"
    )
    check("attach and detach in a loop never runs out of slots", "done" in out, out)
    out = b.exec(
        "for i in range(30):\n    s = board.attach(board.parts.MCP9808(address=0x18 + i % 3))\n"
        "    board.detach(s)\nprint('done')"
    )
    check("nor do I2C devices", "done" in out, out)
    b.exec("board.attach(board.parts.Switch(board.D9))")
    b.reload()
    b.wait("up")
    parts = b.state()["parts"]
    check(
        "run-time parts survive a reload, over the board file's",
        [p["kind"] for p in parts if p["name"] == "D9"] == ["switch"]
        and any(p["name"] == "D5" for p in parts),
        parts,
    )
    b.exec("import microcontroller", interrupt=True)
    b.write("microcontroller.reset()\r")
    b.wait("up", timeout=20)
    parts = b.state()["parts"]
    check(
        "a reset starts from the board files",
        [p["kind"] for p in parts if p["name"] == "D9"] == ["led"]
        and not any(p["name"] == "D5" for p in parts),
        parts,
    )

# Board file lines at one address: the last wins, as everywhere.
with launch('print("up")\n', board='[i2c]\nA = "lis3dh 0x18"\nB = "mcp9808"\n') as b:
    b.wait("up")
    names = sorted({p["name"].split(".")[0] for p in b.state()["parts"]})
    check("two board file devices at one address: the last wins", names == ["B"], names)

print("DONE")
