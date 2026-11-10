"""Parts plugged in from Python: python3 test_parts_api.py /path/to/circuitpython

lab.parts is the catalogue, lab.attach puts a part on the board, and an attached part's
values are its attributes, the same state the panel shows.
"""

import cpapi
from harness import BIN, check, launch

PINS = '[pins]\nD5 = "GPIO16"\nD6 = "GPIO17"\nD7 = "GPIO18"\n'

with launch('print("up")\n', board=PINS) as b:
    b.wait("up")
    names = b.exec("import board, lab\nprint(sorted(dir(lab.parts)))")
    check(
        "catalogue: pin parts, sensors, strips; no display",
        all(n in names for n in ("'BME280'", "'Button'", "'LED'", "'NeoPixel'", "'Potentiometer'"))
        and "SSD1306" not in names,
        names,
    )
    out = b.exec(
        'led = lab.parts.LED(board.D5, color="green")\nprint(repr(led))\nlab.attach(led)\nprint(repr(led))'
    )
    lines = out.splitlines()
    check(
        "an LED: described, then attached",
        len(lines) == 2 and "not attached" in lines[0] and lines[1] == "<LED LED1 on D5: off>",
        out,
    )
    out = b.exec(
        "import digitalio\nd = digitalio.DigitalInOut(board.D5)\nd.switch_to_output(True)\nprint(led.value)"
    )
    check("an LED's value is what the program drives", out.strip() == "True", out)
    out = b.exec(
        'env = lab.parts.BME280(address=0x76, name="ENV")\nlab.attach(env)\nenv.temp = 35\n'
        "i2c = board.I2C()\nprint(i2c.try_lock() and i2c.scan())\ni2c.unlock()\nprint(env.temp)"
    )
    check("a BME280 at 0x76 answers and holds the value set", "118" in out and "35.0" in out, out)
    parts = b.state()["parts"]
    check(
        "the panel shows the set value",
        any(p["name"] == "ENV.temp" and abs(p["value"] - 35) < 0.01 for p in parts),
        parts,
    )
    out = b.exec("e2 = lab.parts.BME280(address=0x76, name='ENV')\nlab.attach(e2)\nprint(e2.temp)")
    check("the same part attached again keeps its values", "35.0" in out, out)
    out = b.exec("print(repr(env))")
    check(
        "a sensor's repr shows its place and values",
        out.startswith("<BME280 ENV on I2C at 0x76: temp=35.00 C"),
        out,
    )
    out = b.exec(
        'try:\n    lab.parts.LED(board.D6).value\nexcept RuntimeError as e:\n    print("err", e)'
    )
    check(
        "a value of a part not attached raises", "err LED" in out and "is not attached" in out, out
    )
    out = b.exec("env.temp = 1000\nprint(env.temp)")
    check("an out-of-range value is clamped as the slider is", "1000" not in out, out)
    out = b.exec(
        "pot = lab.attach(lab.parts.Potentiometer(board.D6))\npot.value = 70000\nprint(pot.value)\n"
        "import analogio\nprint(analogio.AnalogIn(board.D6).value)"
    )
    check(
        "a potentiometer: clamped, and what AnalogIn reads", out.split() == ["65535", "65535"], out
    )
    out = b.exec(
        'try:\n    lab.attach(lab.parts.MCP9808(name="TOOLONGNAME12"))\nexcept ValueError as e:\n    print("err", e)'
    )
    check("a name over the limit is refused, not cut", "err" in out and "11" in out, out)
    out = b.exec(
        'try:\n    lab.attach(lab.parts.Button(board.SCL))\nexcept ValueError as e:\n    print("err", e)'
    )
    check("a bus's pin is refused", "err SCL" in out, out)
    out = b.exec(
        'try:\n    lab.attach(lab.parts.LED(board.D6, color="plaid"))\nexcept ValueError as e:\n    print("err", e)'
    )
    check("a bad option says what the board file would", "err bad colour" in out, out)
    out = b.exec(
        "sw = lab.attach(lab.parts.Switch(board.GPIO4))\nsw.on = True\n"
        "print(sw.on, digitalio.DigitalInOut(board.GPIO4).value)"
    )
    check("a switch set from Python drives its pin", out.strip() == "True True", out)
    out = b.exec(
        'np = lab.attach(lab.parts.NeoPixel(board.D7, layout="2 GRBW"))\nprint(len(np.colors), np.colors[0])'
    )
    check("a NeoPixel strip's colours", out.strip() == "2 (0, 0, 0, 0)", out)
    out = b.exec(
        "import rotaryio\nenc = lab.attach(lab.parts.Encoder(board.GPIO20, board.GPIO21))\n"
        "e = rotaryio.IncrementalEncoder(board.GPIO21, board.GPIO20)\nenc.turn(3)\n"
        "print(repr(enc), e.position, sum('Encoder' in repr(v) for v in lab.attached.values()))"
    )
    check(
        "an encoder on two pins; rotaryio with them swapped counts the other way",
        "on GPIO20, GPIO21: position=3>" in out and out.strip().endswith(" -3 1"),
        out,
    )
    out = b.exec(
        "try:\n    lab.parts.Encoder(board.GPIO22)\nexcept TypeError as e:\n    print('err', e)"
    )
    check("an encoder needs both pins", "err" in out, out)
    out = b.exec(
        "a = lab.attach(lab.parts.Lamp(board.GPIO22))\nc = lab.parts.Lamp(board.GPIO23)\n"
        "d = lab.parts.Lamp(board.GPIO24)\nlab.attach(d)\nlab.attach(c)\nlab.attach(a)\n"
        "print(sorted(n for n in lab.attached if n.startswith('LAMP')))"
    )
    check(
        "a part without name= is numbered by its kind, the next free number when it is attached",
        "['LAMP1', 'LAMP2', 'LAMP3']" in out,
        out,
    )
    out = b.exec("print(repr(lab.parts.BME280))")
    check(
        "a kind's repr lists its keywords and channels",
        "address=0x77" in out and "temp C" in out,
        out,
    )
    out = b.exec(
        "print(repr(lab.parts.LED))\nprint(repr(lab.parts.Button))\nprint(repr(lab.parts.Encoder))"
    )
    check(
        "a kind's repr gives its option's values",
        'LED(pin, *, color="red", name=None)  color: red, green, blue, yellow, orange, white, amber or #rrggbb'
        in out
        and 'pressed: "low" or "high", then key=KEY' in out
        and "Encoder(pin_a, pin_b, *, name=None)" in out,
        out,
    )
    out = b.exec(
        "try:\n    lab.parts.LED(board.GPIO25, colour='blue')\nexcept TypeError as e:\n    print('err', e)"
    )
    check(
        "an unknown keyword says what the kind takes",
        "err" in out and "colour" in out and 'LED(pin, *, color="red", name=None)' in out,
        out,
    )
    b.write("lab.parts.BM\t")
    check("Tab completes the catalogue", "BME280" in b.wait("BME280", timeout=5), "")

# Detach: the place behaves as with the wire pulled; last wins at a place; slots are reused.
PINS3 = PINS + 'A0 = "GPIO19"\nD9 = "GPIO20"\n[led]\nD9 = "red"\n'
with launch('print("up")\n', board=PINS3) as b:
    b.wait("up")
    b.exec(
        'import board, lab, digitalio, analogio\nenv = lab.parts.BME280(address=0x76, name="ENV")\n'
        "lab.attach(env)\nenv.temp = 30\ni2c = board.I2C()"
    )
    out = b.exec(
        'lab.detach("ENV")\ni2c.try_lock()\ntry:\n    i2c.writeto(0x76, bytes([0xd0]))\n'
        'except OSError as e:\n    print("err", e.errno)\ni2c.unlock()\nprint(repr(env))'
    )
    check("a detached I2C device does not answer", "err 19" in out, out)
    check("a detached part says so", "<BME280 ENV on I2C at 0x76, not attached>" in out, out)
    check(
        "the panel leaves a detached device out",
        not any(p["name"].startswith("ENV.") for p in b.state()["parts"]),
        b.state()["parts"],
    )
    out = b.exec("lab.attach(env)\nprint(env.temp)")
    check("attached again, a device keeps its values", out.strip() == "30.0", out)
    out = b.exec(
        "from adafruit_bus_device.i2c_device import I2CDevice\ntry:\n    I2CDevice(i2c, 0x77)\n"
        "    I2CDevice(i2c, 0x77)\nexcept ValueError as e:\n    print('err', e)"
    )
    check("the automatic attaching fills an untouched address", "err" not in out, out)
    b.exec('lab.detach("BME280")')
    out = b.exec("try:\n    I2CDevice(i2c, 0x77)\nexcept ValueError as e:\n    print('err', e)")
    check("an emptied address stays empty", "err" in out, out)
    out = b.exec('try:\n    lab.detach("NOPE")\nexcept ValueError as e:\n    print("err", e)')
    check("detaching an unknown name raises", "err" in out and "NOPE" in out, out)

    b.exec(
        "pot = lab.parts.Potentiometer(board.A0)\nlab.attach(pot)\npot.value = 40000\n"
        "ain = analogio.AnalogIn(board.A0)"
    )
    out = b.exec("lab.detach(pot)\nprint(ain.value)")
    check("an AnalogIn of a pulled potentiometer reads nothing", out.strip() == "0", out)
    out = b.exec("lab.attach(pot)\npot.value = 50000\nprint(ain.value)")
    check("an AnalogIn made before the part was pulled works when it is back", "50000" in out, out)

    out = b.exec(
        "btn = lab.attach(lab.parts.Button(board.D7))\nbtn.pressed = True\n"
        "d7 = digitalio.DigitalInOut(board.D7)\nd7.pull = digitalio.Pull.UP\nprint(d7.value)\n"
        "lab.detach(btn)\nprint(d7.value)"
    )
    check("a pulled button's pin reads its pull", out.split() == ["False", "True"], out)
    b.exec("d7.deinit()")

    b.exec("lab.attach(lab.parts.LED(board.D5))\nlab.attach(lab.parts.Switch(board.D5))")
    kinds = [p["kind"] for p in b.state()["parts"] if p.get("pin") == "GPIO16"]
    check("last wins at a pin", kinds == ["switch"], b.state()["parts"])
    check(
        "the part pulled out is said",
        any("detached: Switch SWITCH1 takes its pin" in l for l in b.stderr),
        b.stderr[-5:],
    )
    b.exec('lab.attach(lab.parts.Touch(board.D6, name="SWITCH1"))')
    names = [
        (p["name"], p["kind"]) for p in b.state()["parts"] if p.get("pin") in ("GPIO16", "GPIO17")
    ]
    check("last wins by name", names == [("SWITCH1", "touch")], names)
    out = b.exec(
        "for i in range(100):\n    l = lab.parts.LED(board.D7)\n    lab.attach(l)\n    lab.detach(l)\nprint('done')"
    )
    check("attach and detach in a loop never runs out of slots", "done" in out, out)
    out = b.exec(
        "for i in range(30):\n    s = lab.attach(lab.parts.MCP9808(address=0x18 + i % 3))\n"
        "    lab.detach(s)\nprint('done')"
    )
    check("nor do I2C devices", "done" in out, out)
    b.exec('lab.attach(lab.parts.Switch(board.D9, name="D9"))')
    b.reload()
    b.wait("up")
    parts = b.state()["parts"]
    check(
        "run-time parts survive a reload, over the board file's",
        [p["kind"] for p in parts if p["name"] == "D9"] == ["switch"]
        and any(p["name"] == "SWITCH1" for p in parts),
        parts,
    )
    b.exec("import microcontroller", interrupt=True)
    b.write("microcontroller.reset()\r")
    b.wait("up", timeout=20)
    parts = b.state()["parts"]
    check(
        "a reset starts from the board files",
        [p["kind"] for p in parts if p["name"] == "D9"] == ["led"]
        and not any(p["name"] == "SWITCH1" for p in parts),
        parts,
    )

# Board file lines at one address: the last wins, as everywhere.
with launch('print("up")\n', board='[i2c]\nA = "lis3dh 0x18"\nB = "mcp9808"\n') as b:
    b.wait("up")
    names = sorted({p["name"].split(".")[0] for p in b.state()["parts"]})
    check("two board file devices at one address: the last wins", names == ["B"], names)

# lab.attached: every part on the board; automatic parts announced on stderr; the panel back.
with launch('print("up")\n', board=PINS + 'D9 = "GPIO20"\n[led]\nD9 = "red"\n') as b:
    b.wait("up")
    out = b.exec("import board, lab\nprint(sorted(lab.attached))\nprint(repr(lab.attached['D9']))")
    check(
        "lab.attached lists the board file's parts",
        "['D9']" in out and "<LED D9 on D9: off>" in out,
        out,
    )
    out = b.exec(
        "from adafruit_bus_device.i2c_device import I2CDevice\nd = I2CDevice(board.I2C(), 0x77)\n"
        "print(repr(lab.attached['BME280']))"
    )
    check("an automatic part is marked", "(automatic)" in out, out)
    check(
        "an automatic part is announced on stderr",
        any(
            'virtual: BME280 attached at I2C 0x77 (automatic; for another chip: [i2c] X = "bmp280")'
            in l
            for l in b.stderr
        ),
        b.stderr[-5:],
    )
    b.exec("import analogio\na = analogio.AnalogIn(board.D7)")
    check(
        "an automatic pin part is announced with its kind",
        any("virtual: Potentiometer D7 attached at D7 (automatic)" in l for l in b.stderr),
        b.stderr[-5:],
    )
    b.exec('lab.attach(lab.parts.Button(board.D5, name="D5"))')
    check(
        "a part attached from Python is not announced",
        not any("D5" in l for l in b.stderr),
        b.stderr[-5:],
    )
    b.exec("lab.attached['BME280'].temp = 30")
    check(
        "an attached part from the dict is a handle",
        any(
            p["name"] == "BME280.temp" and abs(p["value"] - 30) < 0.01 for p in b.state()["parts"]
        ),
        "",
    )
    out = b.exec('lab.detach("D5")\nprint("D5" in lab.attached, hasattr(board, "nothing"))')
    check(
        "a detached part leaves lab.attached; other names stay missing",
        out.strip() == "False False",
        out,
    )
    b.panel("close")
    check("close hides the panel", b.state().get("panel") == "hidden", b.state().get("panel"))
    b.exec("lab.attach(lab.parts.Button(board.D6))")
    check(
        "a part added shows a closed panel again",
        b.state().get("panel") == "shown",
        b.state().get("panel"),
    )
    b.panel("close")
    b.exec("lab.attach(lab.parts.Button(board.D6))")
    check(
        "the same part attached again (code.py on every run) leaves a closed panel closed",
        b.state().get("panel") == "hidden",
        b.state().get("panel"),
    )

# Attached again, renamed, rebuilt from lab.attached: the part as it was, under its new name.
BOARD5 = PINS + 'OLED_PIN = "GPIO21"\n[i2c]\nOLED = "ssd1306"\n'
with launch('print("up")\n', board=BOARD5) as b:
    b.wait("up")
    out = b.exec(
        "import board, lab, digitalio\nfrom adafruit_bus_device.i2c_device import I2CDevice\n"
        "I2CDevice(board.I2C(), 0x77)\n"
        'env = lab.attach(lab.parts.BME280(name="ENV"))\n'
        "print(sorted(lab.attached), repr(env))"
    )
    check(
        "an automatic sensor attached under a name takes it, and is no longer automatic",
        "'ENV'" in out and "'BME280'" not in out and "(automatic)" not in out,
        out,
    )
    out = b.exec('lab.detach("ENV")\nprint("ENV" in lab.attached)')
    check("and is detached by that name", out.strip() == "False", out)

    out = b.exec(
        "servo = lab.attach(lab.parts.Servo(board.D5, name='D5'))\nagain = lab.attached['D5']\n"
        "lab.detach(again)\nlab.attach(again)\nprint(repr(lab.attached['D5']))"
    )
    check("a servo from lab.attached attached again stays a servo", "<Servo D5" in out, out)
    out = b.exec(
        'lab.attach(lab.parts.NeoPixel(board.D6, layout="3x2 RGB"))\nstrip = lab.attached["D6"]\n'
        "lab.detach(strip)\nlab.attach(strip)\nprint(len(strip.colors))"
    )
    check("a strip from lab.attached attached again keeps its layout", out.strip() == "6", out)
    out = b.exec(
        'try:\n    lab.parts.NeoPixel(board.D6, name="RING")\nexcept ValueError as e:\n    print("err", e)'
    )
    check("a strip is named after its pin, and says so", "err" in out, out)

    out = b.exec(
        "d = digitalio.DigitalInOut(board.D7)\nd.switch_to_output(True)\n"
        "led = lab.attach(lab.parts.LED(board.D7))\nprint(led.value)"
    )
    check("an LED attached to a pin the program drives shows it", out.strip() == "True", out)

    out = b.exec('try:\n    lab.detach("OLED")\nexcept ValueError as e:\n    print("err", e)')
    check("a display is not pulled out from Python", "err" in out and "OLED" in out, out)
    out = b.exec(
        "try:\n    lab.attach(lab.parts.MCP9808(address=0x3c))\nexcept ValueError as e:\n    print('err', e)"
    )
    check("nor replaced by a sensor", "err" in out and "OLED" in out, out)

with launch('print("up")\n') as b:
    b.wait("up")
    out = b.exec(
        "import board, lab\nfrom adafruit_bus_device.i2c_device import I2CDevice\n"
        "lab.attach(lab.parts.BME280(address=0x76))\nI2CDevice(board.I2C(), 0x77)\n"
        "print(sorted(lab.attached))"
    )
    check(
        "the automatic attaching never pulls out a part of the same name",
        "'BME280'" in out and "'BME280_2'" in out,
        out,
    )

try:
    cpapi.Board.launch(BIN, "unused", board_file="no-such-board.toml")
    missing = "launched"
except FileNotFoundError as e:
    missing = str(e)
check(
    "a board file that is nowhere is refused before the start",
    "no board file no-such-board.toml" in missing,
    missing,
)
print("DONE")
