"""Virtual I2C devices on the virtual board: python3 test_i2c_virtual.py /path/to/circuitpython

A board file's [i2c] section puts device models on a virtual bus that busio.I2C reaches on any
pins; their values are board panel sliders. The program reads the registers as each device's
library does (the BME280's compensation is the library's); panel commands move the sliders.
"""

from harness import check, launch, run, seen

REGS = """import board, busio, struct, time
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
def rd(addr, reg, n):
    b = bytearray(n)
    i2c.writeto_then_readfrom(addr, bytes([reg]), b)
    return b
def wr(addr, reg, *data):
    i2c.writeto(addr, bytes([reg]) + bytes(data))
"""

# LIS3DH: who am I, the range's divider, z at 1 g lying still, x where the slider is
out, trace = run(
    REGS
    + """print("scan", [hex(a) for a in i2c.scan()])
print("who", hex(rd(0x19, 0x0f, 1)[0]))
wr(0x19, 0x20, 0x07)
wr(0x19, 0x23, 0x88)
print("g2", struct.unpack("<hhh", rd(0x19, 0x28 | 0x80, 6)))
wr(0x19, 0x23, 0xa8)
print("g8", struct.unpack("<hhh", rd(0x19, 0x28 | 0x80, 6)))
for i in range(100):
    x = struct.unpack("<hhh", rd(0x19, 0x28 | 0x80, 6))[0]
    if x != 0:
        break
    time.sleep(0.05)
print("tilted", x)
""",
    "wait g8 (\nset ACCEL.x -0.5\n",  # "g8" alone is in a version's git hash
    "tilted",
    '[i2c]\nACCEL = "lis3dh 0x19"\n',
)
check(
    "LIS3DH: on the bus at its address, WHO_AM_I 0x33",
    "scan ['0x19']" in out and "who 0x33" in out,
    out,
)
check(
    "LIS3DH: still, z is 1 g in the 2 g and the 8 g range",
    "g2 (0, 0, 16380)" in out and "g8 (0, 0, 4096)" in out,
    out,
)
check("LIS3DH: x where the panel's slider is", "tilted -2048" in out, out)
check(
    "trace: the register reads and the slider",
    "I2C 0x19 W 0f" in trace and "PANEL ACCEL.x = -0.50 g" in trace,
    trace,
)

# BME280: the chip id, then temperature, humidity and pressure through the library's compensation
BME = (
    REGS
    + """A = 0x77
print("id", hex(rd(A, 0xd0, 1)[0]))
c = rd(A, 0x88, 24)
t1, t2, t3, p1, p2, p3, p4, p5, p6, p7, p8, p9 = struct.unpack("<HhhHhhhhhhhh", c)
h1 = rd(A, 0xa1, 1)[0]
e = struct.unpack("<hBbBbb", rd(A, 0xe1, 7))
h2 = e[0]; h3 = e[1]; h4 = (e[2] << 4) | (e[3] & 0xf); h5 = (e[4] << 4) | (e[3] >> 4); h6 = e[5]
def measure():
    wr(A, 0xf2, 1); wr(A, 0xf4, 0x25)
    d = rd(A, 0xf7, 8)
    ap = (d[0] << 12) | (d[1] << 4) | (d[2] >> 4); at = (d[3] << 12) | (d[4] << 4) | (d[5] >> 4); ah = (d[6] << 8) | d[7]
    v1 = (at / 16384.0 - t1 / 1024.0) * t2
    v2 = ((at / 131072.0 - t1 / 8192.0) * (at / 131072.0 - t1 / 8192.0)) * t3
    tf = int(v1 + v2)
    v1 = tf / 2.0 - 64000.0
    v2 = v1 * v1 * p6 / 32768.0 + v1 * p5 * 2.0
    v2 = v2 / 4.0 + p4 * 65536.0
    v1 = (p3 * v1 * v1 / 524288.0 + p2 * v1) / 524288.0
    v1 = (1.0 + v1 / 32768.0) * p1
    pr = ((1048576.0 - ap - v2 / 4096.0) * 6250.0) / v1
    pr += (p9 * pr * pr / 2147483648.0 + pr * p8 / 32768.0 + p7) / 16.0
    v1 = tf - 76800.0
    v5 = 1.0 + h3 / 67108864.0 * v1
    v6 = (ah - (h4 * 64.0 + h5 / 16384.0 * v1)) * (h2 / 65536.0) * (v5 * (1.0 + h6 / 67108864.0 * v1 * v5))
    hu = v6 * (1.0 - h1 * v6 / 524288.0)
    return tf / 5120.0, hu, pr / 100
t, h, p = measure()
print("first %.1f %.1f %s" % (t, h, abs(p - 1013.25) < 0.05))
for i in range(100):
    t, h, p = measure()
    if t > 25:
        break
    time.sleep(0.05)
t, h, p = measure()
print("then %.1f %.1f %.1f" % (t, h, p))
"""
)
# The temperature last: once the program sees it, the other two are there.
out, trace = run(
    BME,
    "wait first\nset ENV.press 990\nset ENV.hum 60\nset ENV.temp 30.5\n",
    "then ",
    '[i2c]\nENV = "bme280"\n',
)
check("BME280: chip id 0x60 at 0x77", "id 0x60" in out, out)
check(
    "BME280: 22 C, 45 %, 1013.25 hPa through the compensation", "first 22.0 45.0 True" in out, out
)
check("BME280: what the sliders say", "then 30.5 60.0 990.0" in out, out)

# MCP9808, DS3231 and LC709203F on one bus
out, trace = run(
    REGS
    + """print("ids", rd(0x18, 6, 2).hex(), rd(0x18, 7, 2).hex())
b = rd(0x18, 5, 2)
t = ((b[0] & 0x1f) << 8 | b[1]) / 16.0
print("mcp %.2f" % (t - 256 if b[0] & 0x10 else t))
r = rd(0x68, 0, 7)
bcd = lambda v: (v >> 4) * 10 + (v & 0xf)
print("rtc year", 2000 + bcd(r[6]) >= 2026, bcd(r[5] & 0x1f) <= 12)
wr(0x68, 0, 0x30, 0x59, 0x23, 0x04, 0x31, 0x12, 0x99)
r = rd(0x68, 0, 7)
print("set", r[1:].hex())
def word(cmd):
    b = bytearray(3)
    i2c.writeto_then_readfrom(0x0b, bytes([cmd]), b)
    crc = 0
    for x in bytes([0x16, cmd, 0x17, b[0], b[1]]):
        crc ^= x
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xff if crc & 0x80 else (crc << 1) & 0xff
    return (b[1] << 8 | b[0]) if crc == b[2] else "crc"
print("battery", word(0x0f), word(0x09), word(0x11))
for i in range(100):
    if word(0x0f) != 800:
        break
    time.sleep(0.05)
print("drained", word(0x0f))
""",
    "wait battery\nset BAT.percent 12.5\n",
    "drained",
    '[i2c]\nTEMP = "mcp9808"\nRTC = "ds3231"\nBAT = "lc709203f"\n',
)
check(
    "MCP9808: manufacturer and device ids, 22 C",
    "ids 0054 0400" in out and "mcp 22.00" in out,
    out,
)
check(
    "DS3231: the computer's date, and a date set reads back",
    "rtc year True True" in out and "set 592304311299" in out,
    out,
)
check("LC709203F: 80 %, 3.9 V, a CRC on every word", "battery 800 3900" in out, out)
check("LC709203F: the percentage the slider says", "drained 125" in out, out)

# The CLUE's five sensors: ids, and a value each as its library reads it
out, trace = run(
    REGS
    + """print("ids", hex(rd(0x6a, 0x0f, 1)[0]), hex(rd(0x1c, 0x0f, 1)[0]), hex(rd(0x39, 0x92, 1)[0]), hex(rd(0x77, 0xd0, 1)[0]))
wr(0x6a, 0x12, 0x01)
print("reset clears", rd(0x6a, 0x12, 1)[0] & 1)
wr(0x6a, 0x10, 0x08)
print("lsm6", struct.unpack("<hhh", rd(0x6a, 0x28, 6)))
print("lis3mdl", struct.unpack("<hhh", rd(0x1c, 0x28, 6)))
print("apds", struct.unpack("<HHHH", rd(0x39, 0x94, 8)), rd(0x39, 0x9c, 1)[0])
i2c.writeto(0x44, bytes([0x24, 0x00]))
m = bytearray(6)
i2c.readfrom_into(0x44, m)
def crc(a, b):
    c = 0xff
    for x in (a, b):
        c ^= x
        for _ in range(8):
            c = ((c << 1) ^ 0x31) & 0xff if c & 0x80 else (c << 1) & 0xff
    return c
print("sht", m[0] << 8 | m[1], m[3] << 8 | m[4], crc(m[0], m[1]) == m[2] and crc(m[3], m[4]) == m[5])
print("clue done")
""",
    "",
    "clue done",
    '[i2c]\nA = "lsm6ds33"\nM = "lis3mdl"\nL = "apds9960"\nH = "sht31d"\nP = "bmp280"\n',
)
check(
    "CLUE sensors: LSM6DS33 0x69, LIS3MDL 0x3d, APDS9960 0xab, BMP280 0x58",
    "ids 0x69 0x3d 0xab 0x58" in out,
    out,
)
check(
    "LSM6DS33: reset clears itself, 1 g in the 4 g range",
    "reset clears 0" in out and "lsm6 (0, 0, 8197)" in out,
    out,
)
check("LIS3MDL: 20, 0, -40 uT in the 4 gauss range", "lis3mdl (1368, 0, -2737)" in out, out)
check(
    "APDS9960: clear is red + green + blue, nothing near",
    "apds (900, 300, 300, 300) 0" in out,
    out,
)
check("SHT31-D: 22 C and 45 %, each with its CRC", "sht 25091 29491 True" in out, out)

# A Crickit: seesaw commands as adafruit_seesaw sends them
out, trace = run(
    REGS
    + """def ss(*cmd, n=0):
    i2c.writeto(0x49, bytes(cmd))
    if n:
        b = bytearray(n)
        i2c.readfrom_into(0x49, b)
        return b
print("seesaw", hex(ss(0, 1, n=1)[0]), struct.unpack(">I", ss(0, 2, n=4))[0] >> 16)
ss(1, 3, 0, 0, 0, 4)            # SIGNAL1 (pin 2) an input
ss(1, 0x0b, 0, 0, 0, 4)         # pulled
ss(1, 5, 0, 0, 0, 4)            # up
ss(1, 2, 0, 0x20, 0, 0)         # pin 13 (DRIVE1) an output, low
print("gpio", hex(struct.unpack(">I", ss(1, 4, n=4))[0]))
print("touch", struct.unpack(">H", ss(0x0f, 0x10, n=2))[0])
ss(8, 2, 3, 0, 50)              # SERVO1 at 50 Hz
ss(8, 1, 3, 0x13, 0x33)         # 1.5 ms of 20
print("crickit done")
""",
    "",
    "crickit done",
    '[i2c]\nCRICKIT = "crickit"\n',
)
check("Crickit: a SAMD09 seesaw, product 9999", "seesaw 0x55 9999" in out, out)
check("Crickit: a pulled-up input reads high, a low output low", "gpio 0x4" in out, out)
check("Crickit: an untouched pad reads 300", "touch 300" in out, out)
check(
    "Crickit: a servo's pulse in the trace", "CRICKIT SERVO1 4915/65535 at 50 Hz" in trace, trace
)

# A driver's I2CDevice finds the modelled sensor at its address with no board file; a scan adds nothing
out, trace = run(
    """import board, busio
from adafruit_bus_device.i2c_device import I2CDevice
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
print("scan", i2c.scan())
i2c.unlock()
dev = I2CDevice(i2c, 0x6a)
b = bytearray(1)
with dev as d:
    d.write_then_readinto(bytes([0x0f]), b)
print("whoami", hex(b[0]))
try:
    I2CDevice(i2c, 0x13)
except ValueError as e:
    print("none", e)
print("auto done")
""",
    "",
    "auto done",
    "",
)
check("a scan of an empty bus adds no device", "scan []" in out, out)
check("I2CDevice at 0x6a finds an LSM6DS33 there", "whoami 0x69" in out, out)
check("an address no model has stays empty", "none No I2C device at address: 0x13" in out, out)

# board.I2C() on the virtual board; a bus with no device answers nothing
out, trace = run(
    """import board
i2c = board.I2C()
i2c.try_lock()
print("empty", i2c.scan(), board.STEMMA_I2C() is i2c)
""",
    "",
    "empty",
    "",
)
check("board.I2C() with no devices: an empty scan", "empty [] True" in out, out)

# SHT4x, SCD41 and SCD40, AHT20, MAX17048, DRV2605 and PCA9685, spoken to as their libraries do
SENSIRION = """
def crc(b):
    c = 0xff
    for x in b:
        c ^= x
        for _ in range(8):
            c = ((c << 1) ^ 0x31) & 0xff if c & 0x80 else (c << 1) & 0xff
    return c
def words(addr, cmd, n, size=2):
    i2c.writeto(addr, cmd.to_bytes(size, "big"))
    b = bytearray(3 * n)
    i2c.readfrom_into(addr, b)
    return [b[3 * i] << 8 | b[3 * i + 1] if crc(b[3 * i:3 * i + 2]) == b[3 * i + 2] else "crc" for i in range(n)]
"""
out, trace = run(
    REGS
    + SENSIRION
    + """t, h = words(0x44, 0xfd, 2, 1)
print("sht4x %.1f %.1f" % (-45 + 175 * t / 65535, -6 + 125 * h / 65535), words(0x44, 0x89, 2, 1))
i2c.writeto(0x62, bytes([0x3f, 0x86]))
print("idle ready", words(0x62, 0xe4b8, 1)[0] & 0x7ff != 0)
i2c.writeto(0x62, bytes([0x24, 0x1d, 0x0b, 0xb8, crc(b"\\x0b\\xb8")]))
print("offset", words(0x62, 0x2318, 1), "variant", words(0x62, 0x202f, 1)[0] >> 12)
i2c.writeto(0x62, bytes([0x21, 0xb1]))
print("measuring ready", words(0x62, 0xe4b8, 1)[0] & 0x7ff != 0)
c, t, h = words(0x62, 0xec05, 3)
print("scd41 %d %.1f %.1f" % (c, -45 + 175 * t / 65535, 100 * h / 65535))
try:
    i2c.writeto(0x63, bytes([0x21, 0x9d]))
    print("single shot taken")
except OSError:
    print("scd40 has no single shot", words(0x63, 0x202f, 1)[0] >> 12)
i2c.writeto(0x38, bytes([0xac, 0x33, 0]))
b = bytearray(6)
i2c.readfrom_into(0x38, b)
hum = (b[1] << 12 | b[2] << 4 | b[3] >> 4) * 100 / 0x100000
tmp = ((b[3] & 0xf) << 16 | b[4] << 8 | b[5]) * 200 / 0x100000 - 50
print("aht20 %s %.1f %.1f" % (hex(b[0]), tmp, hum))
v = rd(0x36, 0x08, 2)
cell = struct.unpack(">H", rd(0x36, 0x02, 2))[0] * 78.125 / 1e6
soc = struct.unpack(">H", rd(0x36, 0x04, 2))[0] / 256
try:
    wr(0x36, 0xfe, 0x54, 0x00)
    print("reset acknowledged")
except OSError:
    print("max17048 %s %.2f %.1f reset not acknowledged" % (v.hex(), cell, soc))
print("drv2605", hex(rd(0x5a, 0x00, 1)[0]))
wr(0x5a, 0x04, 47, 0)
wr(0x5a, 0x0c, 1)
print("go after", rd(0x5a, 0x0c, 1)[0])
wr(0x40, 0xfe, 121)
wr(0x40, 0x06, 0, 0, 0x33, 0x01)
for i in range(100):
    c = words(0x62, 0xec05, 3)[0]
    if c != 600:
        break
    time.sleep(0.05)
print("co2 now", c)
""",
    "wait go after\nset CO2.co2 1234\n",
    "co2 now",
    '[i2c]\nSHT = "sht4x"\nCO2 = "scd41"\nCO2B = "scd40 0x63"\nAHT = "aht20"\nBAT = "max17048"\n'
    'HAPTIC = "drv2605"\nPWM = "pca9685"\n',
)
check(
    "SHT4x: 22 C and 45 % on its own humidity scale, a serial number",
    "sht4x 22.0 45.0 [4660, 22136]" in out,
    out,
)
check(
    "SCD41: not ready when idle, ready measuring; a setting read back; variant 1",
    "idle ready False" in out
    and "offset [3000] variant 1" in out
    and "measuring ready True" in out,
    out,
)
check("SCD41: 600 ppm, 22 C, 45 % with CRCs", "scd41 600 22.0 45.0" in out, out)
check(
    "SCD40: a single shot is not acknowledged; variant 0", "scd40 has no single shot 0" in out, out
)
check("SCD41: CO2 where the slider is", "co2 now 1234" in out, out)
check("AHT20: calibrated, 22 C, 45 %", "aht20 0x18 22.0 45.0" in out, out)
check(
    "MAX17048: version 0x0012, 3.9 V, 80 %, its reset not acknowledged",
    "max17048 0012 3.90 80.0 reset not acknowledged" in out,
    out,
)
check(
    "DRV2605: a DRV2605L's status; GO done at once",
    "drv2605 0xe0" in out and "go after 0" in out,
    out,
)
check("trace: the effect played", "HAPTIC plays 47" in trace, trace[-600:])
check(
    "trace: a PCA9685 output as a servo sees it",
    "PWM 0 307/4096 at 50 Hz: 1.50 ms high" in trace,
    trace[-600:],
)

# The automatic chip at an address is the one whose library the driver is, at the address its
# address pins give it too: MCP9808 (not the LIS3DH) at 0x18, a PCA9685 at 0x60 for MotorKit.
DRIVER = """from adafruit_bus_device.i2c_device import I2CDevice
class Driver:
    def __init__(self, i2c, address):
        self.device = I2CDevice(i2c, address)
"""
with launch(
    """import board, lab, busio
import adafruit_mcp9808, adafruit_pca9685
i2c = busio.I2C(board.SCL, board.SDA)
adafruit_mcp9808.Driver(i2c, 0x18)
adafruit_pca9685.Driver(i2c, 0x60)
print("attached", sorted(lab.attached))
""",
    files={"lib/adafruit_mcp9808.py": DRIVER, "lib/adafruit_pca9685.py": DRIVER},
) as b:
    seen(b, "attached", 10)
    out, said = b.console(since=0)["data"], "".join(b.stderr)
check(
    "the driver's chip: an MCP9808 at 0x18, a PCA9685 at 0x60",
    "attached ['MCP9808', 'PCA9685']" in out,
    out,
)
check(
    "and that said on stderr, with the library that chose it",
    'MCP9808 attached at I2C 0x18 (automatic, for adafruit_mcp9808; for another chip: [i2c] X = "lis3dh")'
    in said
    and "PCA9685 attached at I2C 0x60 (automatic, for adafruit_pca9685)" in said,
    said,
)

# The rotary encoder and soil seesaws, HT16K33, IS31FL3741 and TSC2007
out, trace = run(
    REGS
    + """def ss(a, *cmd, n=0):
    i2c.writeto(a, bytes(cmd))
    if n:
        b = bytearray(n)
        i2c.readfrom_into(a, b)
        return b
print("rotary", hex(ss(0x36, 0, 1, n=1)[0]), struct.unpack(">I", ss(0x36, 0, 2, n=4))[0] >> 16)
ss(0x36, 1, 3, 1, 0, 0, 0)       # pin 24 an input
ss(0x36, 1, 0x0b, 1, 0, 0, 0)    # pulled
ss(0x36, 1, 5, 1, 0, 0, 0)       # up
up = struct.unpack(">I", ss(0x36, 1, 4, n=4))[0] >> 24 & 1
ss(0x36, 0x11, 0x30, 0, 0, 0, 7)
print("button up", up, "position set", struct.unpack(">i", ss(0x36, 0x11, 0x30, n=4))[0])
for i in range(100):
    p = struct.unpack(">i", ss(0x36, 0x11, 0x30, n=4))[0]
    down = struct.unpack(">I", ss(0x36, 1, 4, n=4))[0] >> 24 & 1
    if p == -3 and down == 0:
        break
    time.sleep(0.05)
print("turned", p, "pressed", down)
print("soil", struct.unpack(">H", ss(0x37, 0x0f, 0x10, n=2))[0], struct.unpack(">i", ss(0x37, 0, 4, n=4))[0] / 65536)
i2c.writeto(0x70, bytes([0x21]))
i2c.writeto(0x70, bytes([0x00]) + bytes(range(16)))
print("ht16k33", rd(0x70, 0x04, 3).hex())
print("is31 id", hex(rd(0x30, 0xfc, 1)[0]))
wr(0x30, 0xfe, 0xc5)
wr(0x30, 0xfd, 1)
wr(0x30, 0x05, 0x99)
wr(0x30, 0xfd, 0)
print("is31 pages", hex(rd(0x30, 0x05, 1)[0]), hex(rd(0x30, 0xfd, 1)[0]))
wr(0x30, 0xfd, 1)
print("is31 page 1", hex(rd(0x30, 0x05, 1)[0]))
def tsc(fn):
    b = bytearray(2)
    i2c.writeto_then_readfrom(0x48, bytes([fn << 4 | 0x04]), b)
    return b[0] << 4 | b[1] >> 4
print("untouched", tsc(14))
for i in range(100):
    if tsc(14) > 100:
        break
    time.sleep(0.05)
print("touch", tsc(12), tsc(13), tsc(14))
""",
    "wait position set\nset ROT.position -3\nset ROT.button 1\n"
    "wait untouched\nset TS.x 1000\nset TS.y 3000\nset TS.pressure 500\n",
    "touch ",
    '[i2c]\nROT = "rotary"\nSOIL = "soil 0x37"\nLEDS = "ht16k33"\nGLASSES = "is31fl3741"\nTS = "tsc2007"\n',
)
check("rotary: an ATtiny817, product 4991", "rotary 0x87 4991" in out, out)
check(
    "rotary: the button pulled up reads high; a position written reads back",
    "button up 1 position set 7" in out,
    out,
)
check(
    "rotary: the slider turns it, the button pressed reads low", "turned -3 pressed 0" in out, out
)
check("soil: moisture 500, 22 C", "soil 500 22.0" in out, out)
check("HT16K33: its display memory as written", "ht16k33 040506" in out, out)
check("IS31FL3741: its id the address doubled", "is31 id 0x60" in out, out)
check(
    "IS31FL3741: each page its own registers",
    "is31 pages 0x0 0x0" in out and "is31 page 1 0x99" in out,
    out,
)
check(
    "TSC2007: untouched no pressure; x, y and the pressure from the sliders",
    "untouched 0" in out and "touch 1000 3000 500" in out,
    out,
)

# A seesaw at an address no model has is a rotary encoder, as most seesaws are
with launch(
    """import board, lab, busio
import adafruit_seesaw.seesaw
adafruit_seesaw.seesaw.Driver(busio.I2C(board.SCL, board.SDA), 0x4a)
print("attached", sorted(lab.attached))
""",
    files={"lib/adafruit_seesaw/__init__.py": "", "lib/adafruit_seesaw/seesaw.py": DRIVER},
) as b:
    seen(b, "attached", 10)
    out = b.console(since=0)["data"]
check("a seesaw library at 0x4a gets the rotary encoder", "attached ['ROTARY']" in out, out)
print("DONE")
