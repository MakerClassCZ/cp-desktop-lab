"""Real pins through an FT232H: python3 test_ft232h.py /path/to/circuitpython

Skips when no FT232H is found. Tiers by what is wired, through the environment:
  FT232H_LOOP=D4,D5     a wire between two pins (tier 2)
  FT232H_SPI_LOOP=1     D1 wired to D2 (tier 3)
  FT232H_BME280=0x76    a BME280 on SCL/SDA (tier 4)
"""

import os
from harness import Chip, check, fetch_libs

chip = Chip("ft232h", ("no FT232H found", "libusb-1.0 not found"))
run, D = chip.run, chip.D
boot_out = open(os.path.join(D, "boot_out.txt"), errors="replace").read()
check("boot_out.txt names the chosen board", "Board ID:ft232h" in boot_out, boot_out)

# tier 1 A: the board and reads
ok, text = run(
    """import board
print("id", board.board_id)
print("names", sorted(n for n in dir(board) if n[0] in "DC" and n[1:].isdigit()))
print("aliases", board.SCK is board.D0 and board.SCL is board.D0 and board.SDA is board.MOSI and board.MISO is board.D2)
""",
    "aliases ",
)
check(
    "board_id, pin names and aliases",
    "id ft232h" in text
    and "aliases True" in text
    and "names ['C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7', 'D0', 'D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7']"
    in text,
    text,
)
run(
    """import board, digitalio
print("floating", all(digitalio.DigitalInOut(getattr(board, n)).value for n in ("D3", "D4", "D5", "D6", "D7", "C0", "C7")))
""",
    "floating True",
    name="floating inputs high",
)
run(
    """import board, digitalio, time
d = digitalio.DigitalInOut(board.D4)
t = time.monotonic()
for i in range(300):
    d.value
print("reads ok", time.monotonic() - t < 6)
""",
    "reads ok True",
    timeout=12,
    name="300 reads",
)
ok, text = run('import board\nprint("ending")\n', "Code done running", name="clean exit")
check("no lost message", "lost" not in text, text)

# tier 1 B: outputs, pulls, reloads, a second program
run(
    """import board, digitalio
d = digitalio.DigitalInOut(board.D4)
d.switch_to_output(True)
a = d.value
d.value = False
print("readback", a, d.value)
""",
    "readback True False",
    name="output reads back",
)
run(
    """import board, digitalio
d = digitalio.DigitalInOut(board.D4)
try:
    d.switch_to_input(digitalio.Pull.DOWN)
    print("pulldown accepted")
except ValueError as e:
    d.switch_to_input(digitalio.Pull.UP)
    print("pulldown VE", d.value)
""",
    "pulldown VE True",
    name="Pull.DOWN raises, pin stays usable",
)
run(
    """import board, digitalio, supervisor
if supervisor.runtime.run_reason == supervisor.RunReason.STARTUP:
    d = digitalio.DigitalInOut(board.D4)
    d.switch_to_output(False)
    supervisor.reload()
else:
    print("after reload", digitalio.DigitalInOut(board.D5).value, digitalio.DigitalInOut(board.D4).direction == digitalio.Direction.INPUT)
""",
    "after reload True True",
    timeout=15,
    name="soft reload keeps device, pins back to input",
)
chip.after_reset(
    """import board, digitalio, microcontroller
if first:
    digitalio.DigitalInOut(board.D4).switch_to_output(False)
    microcontroller.reset()
else:
    print("after reset", digitalio.DigitalInOut(board.D5).value)
""",
    "after reset True",
    "microcontroller.reset re-opens the device",
)
chip.second_refused("cannot open FT232H")

# tier 2: a wire between two pins
loop = os.environ.get("FT232H_LOOP")
if loop:
    a, b = loop.split(",")
    run(
        """import board, digitalio
a = digitalio.DigitalInOut(board.%s); b = digitalio.DigitalInOut(board.%s)
a.switch_to_output(True); r1 = b.value
a.value = False; r2 = b.value
print("loop", r1, r2)
"""
        % (a, b),
        "loop True False",
        name="loop drives",
    )
    run(
        """import board, digitalio
a = digitalio.DigitalInOut(board.%s); b = digitalio.DigitalInOut(board.%s)
a.switch_to_output(True, drive_mode=digitalio.DriveMode.OPEN_DRAIN); r1 = b.value
a.value = False; r2 = b.value
a.value = True; r3 = b.value
print("od", r1, r2, r3)
"""
        % (a, b),
        "od True False True",
        name="loop open drain releases",
    )
else:
    print("SKIP tier 2 (set FT232H_LOOP=D4,D5)")

# tier 3: SPI with D1 wired to D2
if os.environ.get("FT232H_SPI_LOOP"):
    run(
        """import board, busio
try:
    busio.SPI(board.D4, board.D5, board.D6)
    print("wrong pins accepted")
except ValueError as e:
    spi = busio.SPI(board.SCK, board.MOSI, board.MISO)
    print("wrong pins VE then ok", spi.frequency > 0)
""",
        "wrong pins VE then ok True",
        name="spi wrong pins",
    )
    run(
        """import board, busio
spi = busio.SPI(board.SCK, board.MOSI, board.MISO)
while not spi.try_lock():
    pass
try:
    spi.configure(phase=1)
    print("mode1 accepted")
except ValueError as e:
    print("mode1 VE", "not supported" in str(e))
""",
        "mode1 VE True",
        name="spi mode 1 raises",
    )
    run(
        """import board, busio
spi = busio.SPI(board.SCK, board.MOSI, board.MISO)
while not spi.try_lock():
    pass
spi.configure(baudrate=6000000)
out = bytes(i & 0xFF for i in range(70000)); buf = bytearray(70000)
spi.write_readinto(out, buf)
bad = [i for i in range(70000) if buf[i] != out[i]]
print("loop70000", not bad, bad[:5], spi.frequency)
""",
        "loop70000 True",
        timeout=20,
        name="spi loop 70000 bytes",
    )
    run(
        """import board, busio
spi = busio.SPI(board.SCK, board.MOSI, board.MISO)
while not spi.try_lock():
    pass
buf = bytearray(4)
spi.readinto(buf, write_value=0x5A)
print("fill", bytes(buf) == b"\\x5a" * 4)
""",
        "fill True",
        name="spi readinto write_value",
    )
    run(
        """import board, busio
spi = busio.SPI(board.SCK, board.MOSI, board.MISO)
while not spi.try_lock():
    pass
spi.configure(baudrate=1000000, polarity=1)
out = bytes(range(16)); buf = bytearray(16)
spi.write_readinto(out, buf)
print("mode2", bytes(buf) == out)
""",
        "mode2 True",
        name="spi mode 2",
    )
    ok, text = run(
        """import board, digitalio
spi = board.SPI()
print("singleton", board.SPI() is spi)
while not spi.try_lock():
    pass
out = bytes(range(8)); buf = bytearray(8)
spi.write_readinto(out, buf)
print("board spi", bytes(buf) == out, spi.frequency > 0)
spi.unlock()
spi.deinit()
d = digitalio.DigitalInOut(board.SCK)
try:
    board.SPI()
    print("claimed accepted")
except ValueError:
    print("claimed refused")
""",
        "claimed ",
    )
    check(
        "board.SPI: one object, on the board's own pins; refused while SCK is taken",
        "singleton True" in text and "board spi True True" in text and "claimed refused" in text,
        text,
    )
    run(
        """import board, busio, time
spi = busio.SPI(board.SCK, board.MOSI, board.MISO)
while not spi.try_lock():
    pass
spi.configure(baudrate=2000)
t = time.monotonic()
spi.write(bytes(1500))
elapsed = time.monotonic() - t
spi.configure(baudrate=1000000)
out = bytes(range(4)); buf = bytearray(4)
spi.write_readinto(out, buf)
print("slow write ok", elapsed < 15 and bytes(buf) == out, round(elapsed, 2))
""",
        "slow write ok True",
        timeout=20,
        name="spi slow write beyond the FIFO",
    )
else:
    print("SKIP tier 3 (set FT232H_SPI_LOOP=1 with D1 wired to D2)")

# tier 4: a BME280 on SCL/SDA
bme = os.environ.get("FT232H_BME280")
if bme:
    addr = int(bme, 0)
    run(
        """import board, busio
try:
    busio.I2C(board.D4, board.D5)
    print("wrong pins accepted")
except ValueError as e:
    i2c = busio.I2C(board.SCL, board.SDA)
    print("i2c wrong pins VE then ok")
""",
        "i2c wrong pins VE then ok",
        name="i2c wrong pins",
    )
    ok, text, lines = run(
        """import board, busio
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
buf = bytearray(1)
i2c.writeto_then_readfrom(%d, b"\\xd0", buf)
print("chip id", hex(buf[0]))
"""
        % addr,
        "chip id 0x60",
        name="i2c chip id",
        traced=True,
    )
    check(
        "trace: the chip id read",
        len(lines) == 2
        and lines[0].endswith("I2C 0x%02x W d0 (no stop)" % addr)
        and lines[1].endswith("I2C 0x%02x R 1: 60" % addr),
        "\n".join(lines),
    )
    run(
        """import board, busio
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
try:
    i2c.writeto(0x55, b"\\x00")
    print("nack accepted")
except OSError as e:
    print("nack errno", e.errno)
""",
        "nack errno 19",
        name="i2c nack errno",
    )
    run(
        'import board\nprint("i2c singleton", board.I2C() is board.I2C())\n',
        "i2c singleton True",
        name="board.I2C singleton",
    )
    run(
        """import board, busio
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
short = bytearray(26); long = bytearray(1100)
i2c.writeto_then_readfrom(%d, b"\\x88", short)
i2c.writeto_then_readfrom(%d, b"\\x88", long)
print("long read", long[:26] == short, any(long))
"""
        % (addr, addr),
        "long read True True",
        timeout=20,
        name="i2c long read 1100 bytes",
    )
    run(
        """import board, busio
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
try:
    i2c.writeto(0x55, bytes(1100))
    print("long write accepted")
except OSError as e:
    print("long write errno", e.errno, i2c.scan())
""",
        "long write errno 19 [%d]" % addr,
        timeout=20,
        name="i2c long write to an absent device",
    )
    run(
        """import board, busio
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
cfg = bytearray(1)
i2c.writeto_then_readfrom(%d, b"\\xf5", cfg)
i2c.writeto(%d, bytes([0xF5, cfg[0]]) * 64)
back = bytearray(1)
i2c.writeto_then_readfrom(%d, b"\\xf5", back)
print("long write acked", back == cfg)
"""
        % (addr, addr, addr),
        "long write acked True",
        timeout=20,
        name="i2c long write to the sensor (two batches)",
    )
    fetch_libs(
        os.path.join(D, "lib"),
        ["adafruit-circuitpython-bme280", "adafruit-circuitpython-busdevice"],
    )
    ok, text = run(
        """import board, adafruit_bme280.basic as bme280
s = bme280.Adafruit_BME280_I2C(board.I2C(), address=%d)
t, p, h = s.temperature, s.pressure, s.relative_humidity
print("bme", t, p, h, 10 < t < 40 and 900 < p < 1100 and 5 < h < 95)
"""
        % addr,
        "bme ",
        timeout=15,
    )
    check("bme280 reads", ok and " True" in text.split("bme ")[-1].splitlines()[0], text)
    print([l for l in text.splitlines() if l.startswith("bme ")])
else:
    print("SKIP tier 4 (set FT232H_BME280=0x76)")
print("DONE")
