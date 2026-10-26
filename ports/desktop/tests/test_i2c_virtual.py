"""Virtual I2C devices on the virtual board: python3 test_i2c_virtual.py /path/to/circuitpython

A board file's [i2c] section puts device models on a virtual bus that busio.I2C reaches on any
pins; their values are board panel sliders. The program reads the registers as each device's
library does (the BME280's compensation is the library's); panel commands move the sliders.
"""
from harness import check, run

REGS = '''import board, busio, struct, time
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
def rd(addr, reg, n):
    b = bytearray(n)
    i2c.writeto_then_readfrom(addr, bytes([reg]), b)
    return b
def wr(addr, reg, *data):
    i2c.writeto(addr, bytes([reg]) + bytes(data))
'''

# LIS3DH: who am I, the range's divider, z at 1 g lying still, x where the slider is
out, trace = run(REGS + '''print("scan", [hex(a) for a in i2c.scan()])
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
''', "wait g8\nset ACCEL.x -0.5\n", "tilted", '[i2c]\nACCEL = "lis3dh 0x19"\n')
check("LIS3DH: on the bus at its address, WHO_AM_I 0x33", "scan ['0x19']" in out and "who 0x33" in out, out)
check("LIS3DH: still, z is 1 g in the 2 g and the 8 g range", "g2 (0, 0, 16380)" in out and "g8 (0, 0, 4096)" in out, out)
check("LIS3DH: x where the panel's slider is", "tilted -2048" in out, out)
check("trace: the register reads and the slider", "I2C 0x19 W 0f" in trace and "PANEL ACCEL.x = -0.50 g" in trace, trace)

# BME280: the chip id, then temperature, humidity and pressure through the library's compensation
BME = REGS + '''A = 0x77
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
'''
# The temperature last: once the program sees it, the other two are there.
out, trace = run(BME, "wait first\nset ENV.press 990\nset ENV.hum 60\nset ENV.temp 30.5\n", "then ",
                    '[i2c]\nENV = "bme280"\n')
check("BME280: chip id 0x60 at 0x77", "id 0x60" in out, out)
check("BME280: 22 C, 45 %, 1013.25 hPa through the compensation", "first 22.0 45.0 True" in out, out)
check("BME280: what the sliders say", "then 30.5 60.0 990.0" in out, out)

# MCP9808, DS3231 and LC709203F on one bus
out, trace = run(REGS + '''print("ids", rd(0x18, 6, 2).hex(), rd(0x18, 7, 2).hex())
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
''', "wait battery\nset BAT.percent 12.5\n", "drained", '[i2c]\nTEMP = "mcp9808"\nRTC = "ds3231"\nBAT = "lc709203f"\n')
check("MCP9808: manufacturer and device ids, 22 C", "ids 0054 0400" in out and "mcp 22.00" in out, out)
check("DS3231: the computer's date, and a date set reads back", "rtc year True True" in out and "set 592304311299" in out, out)
check("LC709203F: 80 %, 3.9 V, a CRC on every word", "battery 800 3900" in out, out)
check("LC709203F: the percentage the slider says", "drained 125" in out, out)

# The CLUE's five sensors: ids, and a value each as its library reads it
out, trace = run(REGS + '''print("ids", hex(rd(0x6a, 0x0f, 1)[0]), hex(rd(0x1c, 0x0f, 1)[0]), hex(rd(0x39, 0x92, 1)[0]), hex(rd(0x77, 0xd0, 1)[0]))
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
''', "", "clue done", '[i2c]\nA = "lsm6ds33"\nM = "lis3mdl"\nL = "apds9960"\nH = "sht31d"\nP = "bmp280"\n')
check("CLUE sensors: LSM6DS33 0x69, LIS3MDL 0x3d, APDS9960 0xab, BMP280 0x58", "ids 0x69 0x3d 0xab 0x58" in out, out)
check("LSM6DS33: reset clears itself, 1 g in the 4 g range", "reset clears 0" in out and "lsm6 (0, 0, 8197)" in out, out)
check("LIS3MDL: 20, 0, -40 uT in the 4 gauss range", "lis3mdl (1368, 0, -2737)" in out, out)
check("APDS9960: clear is red + green + blue, nothing near", "apds (900, 300, 300, 300) 0" in out, out)
check("SHT31-D: 22 C and 45 %, each with its CRC", "sht 25091 29491 True" in out, out)

# A Crickit: seesaw commands as adafruit_seesaw sends them
out, trace = run(REGS + '''def ss(*cmd, n=0):
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
''', "", "crickit done", '[i2c]\nCRICKIT = "crickit"\n')
check("Crickit: a SAMD09 seesaw, product 9999", "seesaw 0x55 9999" in out, out)
check("Crickit: a pulled-up input reads high, a low output low", "gpio 0x4" in out, out)
check("Crickit: an untouched pad reads 300", "touch 300" in out, out)
check("Crickit: a servo's pulse in the trace", "CRICKIT SERVO1 4915/65535 at 50 Hz" in trace, trace)

# A driver's I2CDevice finds the modelled sensor at its address with no board file; a scan adds nothing
out, trace = run('''import board, busio
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
''', "", "auto done", "")
check("a scan of an empty bus adds no device", "scan []" in out, out)
check("I2CDevice at 0x6a finds an LSM6DS33 there", "whoami 0x69" in out, out)
check("an address no model has stays empty", "none No I2C device at address: 0x13" in out, out)

# board.I2C() on the virtual board; a bus with no device answers nothing
out, trace = run('''import board
i2c = board.I2C()
i2c.try_lock()
print("empty", i2c.scan(), board.STEMMA_I2C() is i2c)
''', "", "empty", "")
check("board.I2C() with no devices: an empty scan", "empty [] True" in out, out)
print("DONE")
