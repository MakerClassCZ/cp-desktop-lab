"""Fast time: python3 test_fast_time.py /path/to/circuitpython

--fast-time skips the program's sleeps: each returns at once and moves the program's clock on.
Each case prints the program's elapsed time and checks it against the host's.
"""

import os, shutil, struct, sys, time
from harness import BIN, board_file, check, cpapi, drive, launch, refused


def elapsed(code, fast=True, board=None, timeout=20, lib=None, files=None):
    """Runs code.py, which prints "elapsed X"; returns (X, host seconds) or (None, output)."""
    D = drive(code, files)
    if lib:
        shutil.copytree(os.path.join(lib, "asyncio"), os.path.join(D, "lib", "asyncio"))
        shutil.copy(os.path.join(lib, "adafruit_ticks.py"), os.path.join(D, "lib"))
    start = time.monotonic()
    with cpapi.Board.launch(BIN, D, board_file=board_file(board), fast_time=fast) as b:
        try:
            b.wait("elapsed ", timeout=timeout)
            line = b.wait("\n", timeout=5)
            return float(line.strip()), time.monotonic() - start
        except (TimeoutError, ValueError) as e:
            return None, str(e)


def between(name, result, low, high, host_max):
    value, host = result
    check(name, value is not None and low <= value <= high and host < host_max, result)


# The flag in /api/info, through the client's fast_time
with launch('print("Hello")\n', fast_time=True) as b:
    info = b.info()
    check("info: fast_time on", info.get("fast_time") is True, info)
with launch('print("Hello")\n') as b:
    info = b.info()
    check(
        "info: fast_time off, nothing skipped",
        info.get("fast_time") is False and info.get("skipped_ms") == 0,
        info,
    )

# Sleeps
# A sleep counts 1/32768 s: 16 ms is 524 of them, 15.991 ms
between(
    "1000 sleeps of 16 ms",
    elapsed("""import time
t = time.monotonic()
for i in range(1000):
    time.sleep(0.016)
print("elapsed", time.monotonic() - t)
"""),
    15.99,
    17.5,
    5,
)
value, host = elapsed(
    """import time
t = time.monotonic()
time.sleep(1)
print("elapsed", time.monotonic() - t)
""",
    fast=False,
)
check("without the flag a sleep takes host time", value is not None and host >= 1.0, (value, host))

# Interrupting a fast loop, and the REPL
with launch('import time\nprint("go")\nwhile True:\n    time.sleep(1)\n', fast_time=True) as b:
    try:
        b.wait("go")
        b.interrupt()
        b.wait("KeyboardInterrupt", timeout=5)
        check("an interrupt stops a fast sleep loop", True)
    except TimeoutError as e:
        check("an interrupt stops a fast sleep loop", False, e)
    start = time.monotonic()
    try:
        r = b.run(
            "import time\nt = time.monotonic()\ntime.sleep(20)\nprint(time.monotonic() - t)",
            interrupt=True,
            timeout=5,
        )
        check(
            "a sleep in the REPL skips",
            r.get("ok") and float(r.get("output")) >= 20 and time.monotonic() - start < 5,
            r,
        )
    except Exception as e:
        check("a sleep in the REPL skips", False, e)

# After code.py ends nothing more is skipped
with launch('import time\ntime.sleep(2)\nprint("ended")\n', fast_time=True) as b:
    try:
        b.wait("ended")
        b.wait("Code done running", timeout=5)
        first = b.info()["skipped_ms"]
        time.sleep(1)
        second = b.info()["skipped_ms"]
        check(
            "after code.py ends the clock stops jumping",
            first == second and first >= 1900,
            (first, second),
        )
    except TimeoutError as e:
        check("after code.py ends the clock stops jumping", False, e)


# Refused with real hardware
HARDWARE = [
    ("on the ft232h board", ["--board", "ft232h"], None),
    ("with --uart", ["--uart", "COM9" if os.name == "nt" else "/dev/ttyS9"], None),
    (
        "with the computer's microphone",
        [],
        '[pins]\nMIC_DATA = "GPIO18"\n[mic]\nMIC_DATA = "mic"\n',
    ),
]
if sys.platform.startswith("linux"):
    HARDWARE.append(("on the gpiochip board", ["--board", "gpiochip"], None))
for name, args, board in HARDWARE:
    status, out = refused(["--fast-time"] + args, board)
    check("refused " + name, status == 2 and "--fast-time" in out, (status, out[-300:]))

# select.poll's timeout, and asyncio, which waits through it
between(
    "select.poll with nothing registered: a 5 s timeout",
    elapsed("""import select, time
p = select.poll()
t = time.monotonic()
p.poll(5000)
print("elapsed", time.monotonic() - t)
"""),
    5.0,
    5.1,
    3,
)
LIB = os.environ.get("CIRCUITPY_LIB")  # a lib directory with asyncio and adafruit_ticks
if not LIB or not os.path.isdir(os.path.join(LIB, "asyncio")):
    print("SKIP asyncio: set CIRCUITPY_LIB to a lib directory with asyncio and adafruit_ticks")
else:
    between(
        "asyncio: two 5 s sleeps and fifty of 0.1 s side by side",
        elapsed(
            """import asyncio, time
async def long():
    await asyncio.sleep(5)
async def short():
    for i in range(50):
        await asyncio.sleep(0.1)
async def main():
    await asyncio.gather(long(), long(), short())
t = time.monotonic()
asyncio.run(main())
print("elapsed", time.monotonic() - t)
""",
            lib=LIB,
        ),
        5.0,
        5.3,
        3,
    )

# Wall time moves with the program's clock
between(
    "time.time moves with the skipped sleep",
    elapsed("""import time
t = time.time()
time.sleep(30)
print("elapsed", time.time() - t)
"""),
    29,
    31,
    3,
)
between(
    "rtc.RTC set, then moved by a skipped sleep",
    elapsed("""import rtc, time
r = rtc.RTC()
r.datetime = time.struct_time((2030, 1, 1, 12, 0, 0, 1, 1, -1))
time.sleep(10)
print("elapsed", time.mktime(r.datetime) - time.mktime((2030, 1, 1, 12, 0, 0, 1, 1, -1)))
"""),
    9,
    11,
    3,
)
DS = """import board, busio, time
i2c = busio.I2C(board.SCL, board.SDA)
while not i2c.try_lock():
    pass
def seconds():
    b = bytearray(3)
    i2c.writeto_then_readfrom(0x68, bytes([0]), b)
    bcd = lambda v: (v >> 4) * 10 + (v & 0xf)
    return bcd(b[2]) * 3600 + bcd(b[1]) * 60 + bcd(b[0] & 0x7f)
"""
between(
    "the DS3231 set, then moved by a skipped sleep",
    elapsed(
        DS
        + """i2c.writeto(0x68, bytes([0, 0x00, 0x00, 0x12, 0x01, 0x01, 0x01, 0x30]))
time.sleep(10)
print("elapsed", seconds() - 12 * 3600)
""",
        board='[i2c]\nRTC = "ds3231"\n',
    ),
    9,
    11,
    3,
)

# Display frame pacing: in real time too a call now and then skips a frame to catch up (4.5-4.8 s)
between(
    "refresh(target_frames_per_second=10) 50 times: about 5 s",
    elapsed("""import board, time
board.DISPLAY.auto_refresh = False
board.DISPLAY.refresh()
t = time.monotonic()
for i in range(50):
    board.DISPLAY.refresh(target_frames_per_second=10)
print("elapsed", time.monotonic() - t)
"""),
    4.3,
    5.3,
    3,
)

# Sound is silent and plays by the program's clock
SAMPLE = """import array, audiocore, audioio, board, time
sample = audiocore.RawSample(array.array("h", [0] * 8000), sample_rate=8000)     # 1 s
a = audioio.AudioOut(board.SPEAKER)
"""
between(
    "a 2 s sample plays for 2 s of program time",
    elapsed(
        SAMPLE
        + """t = time.monotonic()
a.play(audiocore.RawSample(array.array("h", [0] * 16000), sample_rate=8000))
while a.playing:
    time.sleep(0.01)
print("elapsed", time.monotonic() - t)
"""
    ),
    1.95,
    2.2,
    2,
)
start = time.monotonic()
with launch(
    SAMPLE
    + """a.play(sample, loop=True)
time.sleep(5)
looping = a.playing
a.pause()
time.sleep(5)
paused = a.playing and a.paused
a.resume()
time.sleep(2)
resumed = a.playing and not a.paused
a.stop()
print("states", looping, paused, resumed, not a.playing)
""",
    fast_time=True,
) as b:
    try:
        out = b.wait("states", timeout=10) + b.wait("\n", timeout=5)
        check(
            "a looping sample plays on, pauses, resumes and stops",
            "states True True True True" in out and time.monotonic() - start < 4,
            (out, time.monotonic() - start),
        )
    except TimeoutError as e:
        check("a looping sample plays on, pauses, resumes and stops", False, e)
WAV = bytes(16000 * 2)  # 2 s at 8 kHz, 16-bit mono
WAV = (
    b"RIFF"
    + struct.pack("<I", 36 + len(WAV))
    + b"WAVEfmt "
    + struct.pack("<IHHIIHH", 16, 1, 1, 8000, 16000, 2, 16)
    + b"data"
    + struct.pack("<I", len(WAV))
    + WAV
)
value, host = elapsed(
    SAMPLE
    + """a.play(audiocore.WaveFile(open("/tone.wav", "rb"), bytearray(1024)))
time.sleep(5)
time.sleep(0.01)
print("elapsed", int(a.playing))
""",
    files={"tone.wav": WAV},
)
check("a 2 s WAV read in parts has ended after a 5 s sleep", value == 0, (value, host))
between(
    "a looping sample through a 10 h sleep: the skipped sound is not made",
    elapsed("""import array, audiocore, audioio, board, time
a = audioio.AudioOut(board.SPEAKER)
a.play(audiocore.RawSample(array.array("h", [0] * 4800), sample_rate=48000), loop=True)
t = time.monotonic()
time.sleep(36000)
for i in range(10):
    time.sleep(0.01)            # the background tasks top the sound up
print("elapsed", time.monotonic() - t)
"""),
    36000,
    36001,
    5,
)
between(
    "synthio plays a note for 1 s through it",
    elapsed(
        SAMPLE
        + """import synthio
synth = synthio.Synthesizer(sample_rate=8000)
a.play(synth)
t = time.monotonic()
synth.press(60)
time.sleep(1)
synth.release(60)
time.sleep(0.5)
print("elapsed", time.monotonic() - t)
"""
    ),
    1.5,
    1.6,
    2,
)
