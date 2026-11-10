"""Faults: python3 test_faults.py /path/to/circuitpython

A fault is a panel line, "fault POINT ACTION [filters] [after=N] [count=N|once]": at that point a
call fails, on a count. From the program (lab.panel) its rules end with the run; from the API
they stay. The SD card is pulled out and pushed in with "set SD present 0|1".
"""

import os, subprocess, sys
from harness import BIN, T, check, launch

# Beside the tests when they are copied out, else in the port's tools/
CPAPI = os.path.join(T, "cpapi.py")
if not os.path.exists(CPAPI):
    CPAPI = os.path.join(T, "..", "tools", "cpapi.py")

IMAGE = os.path.join(T, "CIRCUITPY-SD.img")
if os.path.exists(IMAGE):
    os.remove(IMAGE)
BOARD = '[pins]\nSD_CS = "GPIO10"\n[spi]\nSD = "sdcard cs=SD_CS"\n[i2c]\nTEMP = "mcp9808"\n'

SD = """import board, sdcardio, lab
sd = sdcardio.SDCard(board.SPI(), board.SD_CS)
block = bytearray(512)

def write(n):
    try:
        sd.writeblocks(n, block)
        return "ok"
    except OSError as e:
        return "OSError"

def read(n):
    try:
        sd.readblocks(n, block)
        return "ok"
    except OSError as e:
        return "OSError"

lab.panel("fault sd.write error after=1 once")
print("writes", write(100), write(101), write(102))
lab.panel("fault sd.read error block=7")
print("reads", read(6), read(7), read(8), read(7))
print("fired", lab.fired())
lab.panel("fault clear", "fault sd.read timeout once")
print("read timeout", read(9), read(9))
lab.panel("fault sd.write timeout once")
print("write timeout", write(9), write(9))
sd.deinit()
sd = sdcardio.SDCard(board.SPI(), board.SD_CS)
print("after a reset", write(9))
"""

with launch(SD, BOARD) as b:
    out = b.wait("Code done running.", timeout=30)
    check(
        "sd.write after=1 once: the second write fails, the third works",
        "writes ok OSError ok" in out,
        out,
    )
    check(
        "sd.read block=7: that block only, every time", "reads ok OSError ok OSError" in out, out
    )
    check(
        "lab.fired: what fired in the run",
        "fired [('sd.write', 'error', 1), ('sd.read', 'error', 2)]" in out,
        out,
    )
    check(
        "sd.read timeout: no data token comes, the next read works",
        "read timeout OSError ok" in out,
        out,
    )
    check(
        "sd.write timeout: busy for good, until a new SDCard resets the card",
        "write timeout OSError OSError" in out and "after a reset ok" in out,
        out,
    )
    left = b.after_run()
    check(
        "the after-run report has them",
        left["faults_fired"][:2]
        == [
            {"point": "sd.write", "action": "error", "times": 1},
            {"point": "sd.read", "action": "error", "times": 2},
        ],
        left,
    )
    check("the program's rules ended with its run", b.faults()["rules"] == [], b.faults())

PULLED = """import board, sdcardio, lab, time
sd = sdcardio.SDCard(board.SPI(), board.SD_CS)
lab.panel("set SD present 0")
try:
    sd.readblocks(0, bytearray(512))
    print("read pulled out")
except OSError:
    print("pulled out: OSError")
lab.panel("set SD present 1")
sd.deinit()
sd = sdcardio.SDCard(board.SPI(), board.SD_CS)
sd.readblocks(0, bytearray(512))
print("pushed in: read", sd.count() > 0)
"""
with launch(PULLED, BOARD) as b:
    out = b.wait("Code done running.", timeout=30)
    check(
        "set SD present 0: the card answers nothing; 1: a new SDCard reads it",
        "pulled out: OSError" in out and "pushed in: read True" in out,
        out,
    )
    out = b.exec(
        "import lab\ntry:\n    lab.panel('set SD present 2')\nexcept ValueError as e:\n    print('err', e)"
    )
    check("set SD present takes 0 or 1", "err an SD card takes: set SD present 0|1" in out, out)

I2C = """import board, busio, errno, lab
i2c = busio.I2C(board.SCL, board.SDA)
i2c.try_lock()

def write(addr):
    try:
        i2c.writeto(addr, bytes([5]))
        return "ok"
    except OSError as e:
        return {errno.ENODEV: "ENODEV", errno.ETIMEDOUT: "ETIMEDOUT"}.get(e.errno, e.errno)

lab.panel("fault i2c nack addr=0x19")
print("other address", write(0x18))
lab.panel("fault clear", "fault i2c nack addr=0x18 once")
print("nack", write(0x18), write(0x18))
lab.panel("fault i2c hold")
print("hold", write(0x18))
"""
with launch(I2C, BOARD) as b:
    out = b.wait("Code done running.", timeout=30)
    check("i2c nack addr=: other addresses answer", "other address ok" in out, out)
    check("i2c nack once: ENODEV, then the device answers", "nack ENODEV ok" in out, out)
    check("i2c hold: the bus times out", "hold ETIMEDOUT" in out, out)
    trace = b.trace(since=0)["data"]
    check(
        "each firing is in the trace",
        "FAULT i2c 0x18: nack" in trace and "FAULT i2c 0x18: hold" in trace,
        trace[-600:],
    )

# From outside: the rules stay across reloads, with their counts; the run clears only its own.
OUTSIDE = """import board, busio, lab
i2c = busio.I2C(board.SCL, board.SDA)
i2c.try_lock()
lab.panel("fault i2c hold addr=0x40", "fault clear")
try:
    i2c.writeto(0x18, bytes([5]))
    print("answered")
except OSError:
    print("no answer")
"""
with launch(OUTSIDE, BOARD) as b:
    b.wait("Code done running.", timeout=30)
    b.panel("fault i2c nack addr=0x18 count=2")
    runs = []
    for i in range(3):
        b.reload()
        runs.append(b.wait("Code done running.", timeout=30))
    rules = b.faults()["rules"]
    check(
        "an API rule stays across reloads and counts on: two runs fail, the third does not",
        ["no answer" in o for o in runs] == [True, True, False],
        runs,
    )
    check(
        "the program's fault clear leaves the API's rule",
        len(rules) == 1
        and rules[0]["rule"] == "fault i2c nack addr=0x18 count=2"
        and rules[0]["fired"] == 2,
        rules,
    )
    for line, said in (
        ("fault nowhere x", "fault needs a point: sd.read sd.write i2c"),
        ("fault i2c explode", "i2c does: nack or hold"),
        ("fault i2c nack port=1", "not port="),
    ):
        try:
            b.panel(line)
            got = "applied"
        except Exception as e:
            got = str(e)
        check("a wrong fault line says what is wrong: " + line, said in got, got)

# ab counts the faults fired.
path = os.path.join(T, "faults_ab.py")
open(path, "w").write(I2C.replace('lab.panel("fault i2c hold")\nprint("hold", write(0x18))\n', ""))
board = os.path.join(T, "faults_board.toml")
open(board, "w").write(BOARD)
try:
    r = subprocess.run(
        [
            sys.executable,
            CPAPI,
            "ab",
            BIN,
            BIN,
            path,
            "--names",
            "one,two",
            "--runs",
            "2",
            "--board-file",
            board,
        ],
        capture_output=True,
        text=True,
        timeout=300,
    )
finally:
    os.remove(path)
    os.remove(board)
check(
    "ab: a row of the faults fired",
    "| faults fired | i2c nack x2 | i2c nack x2 |" in r.stdout,
    r.stdout + r.stderr[-500:],
)
