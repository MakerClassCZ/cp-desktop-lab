"""Real pins through an MCP2221A: python3 test_mcp2221.py /path/to/circuitpython

Skips when no MCP2221 is found. Tiers by what is wired, through the environment:
  MCP2221_LOOP=G2,G3     a wire between two GPIO (tier 2: digital and analog loopback)
  MCP2221_PCF8523=0x68   a PCF8523 clock on the chip's SCL/SDA, e.g. on STEMMA QT (tier 3)
"""

import os
from harness import Chip, check, fetch_libs

chip = Chip("mcp2221", ("no MCP2221 found",))
run, D = chip.run, chip.D

# tier 1: the board, GPIO, the buses' refusals, reloads
ok, text = run(
    'import board\nprint("id", board.board_id)\n'
    'print("names", sorted(n for n in dir(board) if n[0] in "GDSI" and not n.startswith("DISPLAY")))\n',
    "names [",
)
check(
    "board_id and pin names",
    "id mcp2221" in text and "names ['G0', 'G1', 'G2', 'G3', 'I2C', 'SCL', 'SDA']" in text,
    text,
)
run(
    """import board, digitalio
try:
    digitalio.DigitalInOut(board.SCL)
    print("scl accepted")
except ValueError as e:
    print("scl VE", "Invalid pin" in str(e))
""",
    "scl VE True",
    name="digitalio on SCL raises",
)
run(
    """import board, digitalio
d = digitalio.DigitalInOut(board.G2)
try:
    d.switch_to_input(digitalio.Pull.UP)
    print("pull accepted")
except ValueError as e:
    print("pull VE")
try:
    d.switch_to_output(True, drive_mode=digitalio.DriveMode.OPEN_DRAIN)
    print("od accepted")
except ValueError as e:
    print("od VE")
""",
    "od VE",
    name="Pull.UP and open drain raise",
)
run(
    """import board, digitalio
d = digitalio.DigitalInOut(board.G2)
d.switch_to_output(True)
a = d.value
d.value = False
print("readback", a, d.value)
""",
    "readback True False",
    name="output reads back",
)
run(
    """import board, digitalio, supervisor
if supervisor.runtime.run_reason == supervisor.RunReason.STARTUP:
    digitalio.DigitalInOut(board.G2).switch_to_output(True)
    supervisor.reload()
else:
    d = digitalio.DigitalInOut(board.G2)
    d.switch_to_output(False)
    print("after reload", d.value)
""",
    "after reload False",
    timeout=15,
    name="soft reload keeps the device",
)
run(
    """import board, busio, digitalio, supervisor
if supervisor.runtime.run_reason == supervisor.RunReason.STARTUP:
    for i in range(20):
        busio.I2C(board.SCL, board.SDA).deinit()
    keep = busio.I2C(board.SCL, board.SDA)
    g = [digitalio.DigitalInOut(p) for p in (board.G0, board.G1, board.G2, board.G3)]
    for d in g:
        d.switch_to_output(True)
    print("deinit ok", [d.value for d in g])
    supervisor.reload()
else:
    i2c = busio.I2C(board.SCL, board.SDA)
    g = [digitalio.DigitalInOut(p) for p in (board.G0, board.G1)]
    for d in g:
        d.switch_to_output(False)
    print("after i2c reload", [d.value for d in g])
""",
    "after i2c reload [False, False]",
    timeout=20,
    name="i2c deinit and a reload with i2c open",
)
chip.after_reset(
    """import board, digitalio, microcontroller
if first:
    microcontroller.reset()
else:
    d = digitalio.DigitalInOut(board.G2); d.switch_to_output(True)
    print("after reset", d.value)
""",
    "after reset True",
    "microcontroller.reset re-opens the device",
)
chip.second_refused("cannot open MCP2221")
run(
    """import board, busio
try:
    busio.SPI(board.G0, board.G1, board.G2)
    print("spi built")
except Exception as e:
    print("spi", type(e).__name__, e)
""",
    "spi RuntimeError busio.SPI needs --board ft232h",
    name="spi raises on this board",
)
run(
    """import board, busio
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
try:
    i2c.writeto(0x55, b"\\x00")
    print("absent accepted")
except OSError as e:
    print("absent errno", e.errno, "scan ok", 0x55 not in i2c.scan())
""",
    "absent errno 19 scan ok True",
    timeout=20,
    name="i2c absent address errno 19",
)
run(
    """import board, analogio, digitalio, supervisor
if supervisor.runtime.run_reason == supervisor.RunReason.STARTUP:
    a = analogio.AnalogIn(board.G1)
    v = a.value
    supervisor.reload()
else:
    d = digitalio.DigitalInOut(board.G1); d.switch_to_output(True)
    print("clean", d.value)
""",
    "clean True",
    timeout=15,
    name="second run starts clean after analog",
)

ok, text, lines = run(
    """import board, busio
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
try:
    i2c.writeto(0x55, bytes([1, 2, 0xab]))
except OSError:
    pass
i2c.scan()
print("traced")
""",
    "traced",
    timeout=20,
    traced=True,
)
check(
    "trace: a write to an absent address",
    any(
        l.split()[1:] == ["I2C", "0x55", "W", "01", "02", "ab", "->", "NACK"]
        for l in lines
        if len(l.split()) > 1
    ),
    "\n".join(lines[:5]),
)
check(
    "trace: the scan's probes",
    sum(1 for l in lines if " probe" in l) >= 100
    and any(l.endswith("0x55 probe -> NACK") for l in lines),
    "\n".join(lines[-3:]),
)
check(
    "trace: seconds first",
    all(l.split()[0].replace(".", "").isdigit() for l in lines if l.strip()),
    "\n".join(lines[:3]),
)

# tier 2: a wire between two GPIO
loop = os.environ.get("MCP2221_LOOP")
if loop:
    a, b = loop.split(",")
    # Both ways: an input with nothing on it follows its driven neighbour on this board, so one
    # direction alone passes without the wire.
    for x, y in ((a, b), (b, a)):
        run(
            """import board, digitalio
a = digitalio.DigitalInOut(board.%s); b = digitalio.DigitalInOut(board.%s)
a.switch_to_output(True); r1 = b.value
a.value = False; r2 = b.value
print("loop", r1, r2)
"""
            % (x, y),
            "loop True False",
            name="loop drives %s -> %s" % (x, y),
        )
        run(
            """import board, digitalio, analogio
a = digitalio.DigitalInOut(board.%s); b = analogio.AnalogIn(board.%s)
a.switch_to_output(True); hi = b.value
a.value = False; lo = b.value
print("adc", hi > 60000, lo < 5000, hi, lo)
"""
            % (x, y),
            "adc True True",
            name="AnalogIn follows the driven pin %s -> %s" % (x, y),
        )
    run(
        """import board, analogio
out = analogio.AnalogOut(board.%s); inp = analogio.AnalogIn(board.%s)
out.value = 65535; hi = inp.value
out.value = 0; lo = inp.value
print("dac", hi > 55000, lo < 5000, hi, lo)
"""
        % (b, a),
        "dac True True",
        name="AnalogOut read back",
    )
    run(
        """import board, analogio, digitalio
d = digitalio.DigitalInOut(board.%s)
try:
    analogio.AnalogIn(board.%s)
    print("claimed accepted")
except ValueError as e:
    print("claimed VE")
"""
        % (b, b),
        "claimed VE",
        name="analog on a claimed pin",
    )
else:
    print("SKIP tier 2 (set MCP2221_LOOP=G2,G3)")

# tier 2b: the chip's own serial port with TX wired to RX
if os.environ.get("MCP2221_UART_LOOP"):
    run(
        """import board, busio
uart = busio.UART(board.TX, board.RX, baudrate=115200, timeout=1)
uart.reset_input_buffer()
uart.write(b"loop 123")
print("uart", uart.read(8))
uart.deinit()
u = board.UART()
u.baudrate = 9600
u.write(b"slow")
print("board uart", u.read(4))
""",
        "board uart b'slow'",
        timeout=15,
        name="uart through the chip's serial port, TX wired to RX",
    )
else:
    print("SKIP tier 2b (set MCP2221_UART_LOOP=1 with TX wired to RX)")

# tier 3: a PCF8523 real-time clock on the chip's bus (STEMMA QT)
rtc = os.environ.get("MCP2221_PCF8523")
if rtc:
    addr = int(rtc, 0)
    ok, text, lines = run(
        """import board, busio
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
regs = bytearray(3)
i2c.writeto_then_readfrom(%d, b"\\x00", regs)
print("traced", " ".join("%%02x" %% b for b in regs))
"""
        % addr,
        "traced",
        traced=True,
    )
    w = [l for l in lines if " I2C 0x%02x W 00 (no stop)" % addr in l]
    r = [l for l in lines if " I2C 0x%02x R 3: " % addr in l]
    check(
        "trace: write without stop, then the read with its bytes",
        len(w) == 1 and len(r) == 1 and lines.index(w[0]) + 1 == lines.index(r[0]),
        "\n".join(lines),
    )
    run(
        """import board, busio
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
before = bytearray(20); long = bytearray(1100); after = bytearray(20)
i2c.writeto_then_readfrom(%d, b"\x00", before)
i2c.writeto_then_readfrom(%d, b"\x00", long)
i2c.writeto_then_readfrom(%d, b"\x00", after)
# the register pointer wraps every 20 bytes; the clock may tick meanwhile
same = sum(1 for i in range(1100) if long[i] in (before[i %% 20], after[i %% 20]))
print("long read", same == 1100, same)
"""
        % (addr, addr, addr),
        "long read True",
        timeout=20,
        name="i2c long read 1100 bytes",
    )
    run(
        """import board, busio
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
regs = bytearray(20)
i2c.writeto_then_readfrom(%d, b"\x00", regs)
# 127 bytes from register 0: every register gets its own value back (127 = 6 * 20 + 7)
i2c.writeto(%d, b"\x00" + bytes(regs) * 6 + bytes(regs[:7]))
back = bytearray(20)
i2c.writeto_then_readfrom(%d, b"\x00", back)
print("long write", back[0:3] == regs[0:3] and back[6:] == regs[6:])
"""
        % (addr, addr, addr),
        "long write True",
        timeout=20,
        name="i2c long write to the clock (three batches)",
    )
    fetch_libs(
        os.path.join(D, "lib"),
        [
            "adafruit-circuitpython-pcf8523",
            "adafruit-circuitpython-register",
            "adafruit-circuitpython-busdevice",
        ],
    )
    ok, text = run(
        """import board, adafruit_pcf8523.pcf8523 as pcf
rtc = pcf.PCF8523(board.I2C())
t = rtc.datetime
print("rtc", t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 2000 <= t.tm_year <= 2099 and 1 <= t.tm_mon <= 12)
""",
        "rtc ",
        timeout=15,
    )
    check(
        "pcf8523 library reads the clock",
        ok and " True" in text.split("rtc ")[-1].splitlines()[0],
        text,
    )
    print([l for l in text.splitlines() if l.startswith("rtc ")])
else:
    print("SKIP tier 3 (set MCP2221_PCF8523=0x68)")
print("DONE")
