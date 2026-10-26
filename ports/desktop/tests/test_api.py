"""The control API: python3 test_api.py /path/to/circuitpython

The program started with --api 0 through the client (tools/cpapi.py), headless, and driven only
through the API: no sleeps to wait for the program.
"""

import http.client, os, socket, subprocess, threading, time
from harness import BIN, HEADLESS, T, check, cpapi, drive, launch, picture


def raw(board, method, path, headers):
    host, port = board.url.split("//")[1].rstrip("/").split(":")
    c = http.client.HTTPConnection(host, int(port), timeout=5)
    c.request(method, path, headers=headers)
    r = c.getresponse()
    r.read()
    c.close()
    return r.status


# Start, info, authentication, quit
started = time.monotonic()
with launch('print("Hello")\n') as b:
    b.wait("Hello")
    # A desktop has no reset button to wait for, nor a battery that may fail while boot_out.txt is written.
    check(
        "code.py runs within a second of the start",
        time.monotonic() - started < 1.0,
        time.monotonic() - started,
    )
    info = b.info()
    check(
        "info names the board, the process and its boot",
        info.get("board") == "virtual"
        and info.get("pid")
        and info.get("boot")
        and "status" in info,
        info,
    )
    check("no token: 401", raw(b, "GET", "/api/info", {}) == 401)
    check(
        "a wrong token: 401", raw(b, "GET", "/api/info", {"Authorization": "Bearer nope"}) == 401
    )
    check(
        "another Host: 403",
        raw(
            b,
            "GET",
            "/api/info",
            {"Authorization": "Bearer " + b.token, "Host": "evil.example:80"},
        )
        == 403,
    )
    check(
        "OPTIONS: 405, no CORS",
        raw(b, "OPTIONS", "/api/info", {"Origin": "http://evil.example"}) == 405,
    )
    p = b.process
    b.quit()
    try:
        status = p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        status = None
    check("quit ends the program with status 0", status == 0, status)

# The console: waiting for text, typing, interrupting, reloading, the status
LOOP = 'import time\nprint("Hello")\nwhile True:\n    time.sleep(0.01)\n'
with launch(LOOP) as b:
    try:
        b.wait("Hello")
        check("wait sees code.py's output", True)
    except TimeoutError as e:
        check("wait sees code.py's output", False, e)
    check("status: code.py while it runs", b.info()["status"] == "code.py", b.info())
    b.interrupt()
    b.wait("KeyboardInterrupt")
    b.wait("Press any key")
    b.write("\r")
    out = b.wait(">>> ")
    check(
        "interrupt stops the program, a key enters the REPL", "Adafruit CircuitPython" in out, out
    )
    b.write("6*7\r")
    out = b.wait(">>> ")
    check("a typed line runs", "42" in out, out)
    check("status: REPL at the prompt", b.info()["status"] == "REPL", b.info())
    b.reload()
    try:
        b.wait("Hello")
        check("reload from the REPL runs code.py again", True)
    except TimeoutError as e:
        check("reload from the REPL runs code.py again", False, e)
    b.reload()
    try:
        b.wait("Hello")
        check("reload while code.py runs starts it again", True)
    except TimeoutError as e:
        check("reload while code.py runs starts it again", False, e)
    b.read(timeout=0.5)
    t = time.time()
    r = b.console(since=b.offset, timeout=0)
    check("nothing new: an empty answer at once", r["data"] == "" and time.time() - t < 0.5, r)
    t = time.time()
    r = b.console(since=b.offset, timeout=0.6)
    check(
        "nothing new with a timeout: empty after it",
        r["data"] == "" and 0.5 < time.time() - t < 2,
        (r, time.time() - t),
    )

# exec through the raw REPL
with launch(LOOP) as b:
    b.wait("Hello")
    try:
        b.exec("x = 1")
        check("exec while code.py runs: 409", False, "no error")
    except cpapi.ApiError as e:
        check("exec while code.py runs: 409", e.status == 409 and "running" in e.message, e)
    check("interrupt=True stops it and runs", b.eval("6*7", interrupt=True) == "42")
    b.exec("x = 5")
    check("the REPL keeps its variables between execs", b.eval("x") == "5")
    out = b.exec("print('a'); print('b')")
    check("statements: what they printed", out.replace("\r\n", "\n") == "a\nb\n", repr(out))
    check("a call returning None: no value", b.eval("print('hi')") is None)
    check("a string's repr", b.eval("'é' + 'x'") == "'éx'", b.eval("'é' + 'x'"))
    try:
        b.exec("1/0")
        check("an exception comes back with its traceback", False)
    except cpapi.CircuitPythonError as e:
        check(
            "an exception comes back with its traceback", "ZeroDivisionError" in e.error, e.error
        )
    try:
        b.exec("while True:\n    pass\n", timeout=1)
        check("a run past its timeout is stopped", False)
    except cpapi.CircuitPythonError as e:
        check("a run past its timeout is stopped", e.error == "timeout", e.error)
    check("and the REPL works after it", b.eval("1+1") == "2")
    long_list = "[" + ",".join(str(i) for i in range(1200)) + "]"
    check(
        "5 KB of code arrives whole",
        b.eval("sum(%s)" % long_list) == str(sum(range(1200))),
        len(long_list),
    )
    check(
        "quotes, backslashes and newlines in the code",
        b.eval("'a\\'b\\\\c' + '''\nd'''") == repr("a'b\\c\nd"),
        b.eval("'a\\'b\\\\c' + '''\nd'''"),
    )
    # A client that gives up during an exec: the next exec still runs
    try:
        b.request("POST", "/api/exec", body={"code": "import time; time.sleep(2)"}, timeout=0.5)
    except Exception:
        pass
    check(
        "an exec abandoned by its client does not block the next", b.eval("3*3", timeout=10) == "9"
    )
    b.write("7*6\r")
    check(
        "the friendly REPL answers after execs",
        "42" in b.wait(">>> ", timeout=5) or "42" in b.wait(">>> ", timeout=5),
    )

# exec when code.py has ended: no interrupt needed
with launch('print("Hello")\n') as b:
    b.wait("Press any key")
    stat = "/proc/%d/stat" % b.info()["pid"]
    if os.path.exists(stat):
        # Waiting for a key, with the display window open: the program sleeps (once it spun a core).
        def cpu():
            fields = open(stat).read().rsplit(")", 1)[1].split()
            return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")

        before, t = cpu(), time.monotonic()
        time.sleep(2)
        share = (cpu() - before) / (time.monotonic() - t)
        check("after code.py ends the program idles: under 15% of a core", share < 0.15, share)
    check("exec after code.py ended (Done)", b.eval("2**10") == "1024")

# The panel: commands applied before the answer, the state as the panel shows it
# (test_panel and test_neopixel go through every kind of part)
PARTS_BOARD = """[pins]
BUTTON = "GPIO14"
LED = "GPIO15"

[button]
BUTTON = ""
[led]
LED = "green"
[i2c]
TEMP = "mcp9808"
"""
PARTS_CODE = """import board, digitalio, time
led = digitalio.DigitalInOut(board.LED); led.switch_to_output(True)
button = digitalio.DigitalInOut(board.BUTTON); button.pull = digitalio.Pull.UP
print("ready")
last = None
while True:
    if button.value != last:
        print("button", "up" if button.value else "pressed")
        last = button.value
    time.sleep(0.01)
"""
with launch(PARTS_CODE, PARTS_BOARD) as b:
    b.wait("ready")
    check("the LED code.py lit reads on", b.part("LED")["on"] is True, b.part("LED"))
    check(
        "board.DISPLAY in the displays",
        any(x["name"] == "DISPLAY" for x in b.state()["displays"]),
        b.state()["displays"],
    )
    b.press("BUTTON")
    try:
        b.wait("button pressed", timeout=5)
        check("press reaches the program, no sleep", True)
    except TimeoutError as e:
        check("press reaches the program, no sleep", False, e)
    b.set("TEMP.temp", 29.5)
    check(
        "a sensor set reads back",
        abs(b.part("TEMP.temp")["value"] - 29.5) < 0.01,
        b.part("TEMP.temp"),
    )
    try:
        b.panel("press NOPE")
        check("a part that is not there: 400 naming it", False)
    except cpapi.ApiError as e:
        check(
            "a part that is not there: 400 naming it", e.status == 400 and "NOPE" in e.message, e
        )
    # The design's finish line: the value set on the panel, read through the REPL
    b.exec("import board; i2c = board.I2C(); i2c.try_lock(); buf = bytearray(2)", interrupt=True)
    b.exec("i2c.writeto_then_readfrom(0x18, bytes([5]), buf)")
    v = b.eval("(((buf[0] & 0x1f) << 8) | buf[1]) / 16")
    check("the sensor's value through the REPL", v is not None and abs(float(v) - 29.5) < 0.1, v)

# Displays as PNG
RED = """import board, displayio
bitmap = displayio.Bitmap(board.DISPLAY.width, board.DISPLAY.height, 1)
palette = displayio.Palette(1); palette[0] = 0xff0000
group = displayio.Group(); group.append(displayio.TileGrid(bitmap, pixel_shader=palette))
board.DISPLAY.root_group = group
print("drawn")
while True:
    pass
"""
with launch(RED, '[i2c]\nOLED = "ssd1306 128x64"\n') as b:
    b.wait("drawn")
    for _ in range(40):
        w, h, px = picture(b)
        if px(160, 120) == (255, 0, 0):
            break
        time.sleep(0.05)
    check(
        "board.DISPLAY as a PNG: 320x240, red",
        (w, h, px(160, 120)) == (320, 240, (255, 0, 0)),
        (w, h, px(160, 120)),
    )
    w, h, px = picture(b, "OLED")
    check("a board file's display by name", (w, h) == (128, 64), (w, h))
    try:
        b.display("NOPE")
        check("an unknown display: 404", False)
    except cpapi.ApiError as e:
        check("an unknown display: 404", e.status == 404, e)

# The bus trace as a stream, with --trace-bus or without
TRACED = """import board, digitalio, time
led = digitalio.DigitalInOut(board.LED); led.switch_to_output(False)
i2c = board.I2C(); i2c.try_lock(); buf = bytearray(2)
i2c.writeto_then_readfrom(0x18, bytes([5]), buf)
print("traced")
while True:
    led.value = not led.value
    time.sleep(0.05)
"""
TRACE_FILE = os.path.join(T, "api_trace.txt")
if os.path.exists(TRACE_FILE):
    os.remove(TRACE_FILE)
for args in ((), ("--trace-bus", TRACE_FILE)):
    with launch(TRACED, PARTS_BOARD, args=args) as b:
        b.wait("traced")
        how = " (and --trace-bus)" if args else ""
        try:
            b.trace_wait("I2C 0x18")
            check("the trace has the I2C read" + how, True)
        except TimeoutError as e:
            check("the trace has the I2C read" + how, False, e)
        try:
            b.trace_wait("LED LED on")
            b.trace_wait("LED LED off")
            check("the trace follows the LED" + how, True)
        except TimeoutError as e:
            check("the trace follows the LED" + how, False, e)
check(
    "--trace-bus still gets the lines",
    os.path.exists(TRACE_FILE) and "I2C 0x18" in open(TRACE_FILE).read(),
)

# A reset: the program starts again on the same port, with the same token, and a reader
# waiting on the console meanwhile gets an answer
with launch(LOOP) as b:
    b.wait("Hello")
    first = b.info()
    waited = []

    def read_long():
        try:
            waited.append(
                b.request("GET", "/api/console?since=999999999&timeout=8000", timeout=30)
            )
        except Exception as e:
            waited.append(e)

    reader = threading.Thread(target=read_long, daemon=True)
    b.exec("import microcontroller", interrupt=True)
    reader.start()
    time.sleep(0.5)
    b.write("microcontroller.reset()\r")
    try:
        b.wait("Hello", timeout=20)
        check("after a reset the client reaches the new program on the same port", True)
    except Exception as e:
        check("after a reset the client reaches the new program on the same port", False, repr(e))
    reader.join(15)
    check(
        "the reader waiting through the reset is answered",
        waited and not reader.is_alive(),
        waited,
    )
    again = b.info()
    check("a new boot id after the reset", again["boot"] != first["boot"], (first, again))
    check("exec works after the reset", b.eval("6*7", interrupt=True) == "42")
    p = b.process
    b.quit()
    try:
        status = p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        status = None
    check("quit after a reset ends the program (and on Windows its parent)", status == 0, status)
    if os.name == "nt" and status == 0:
        left = subprocess.run(
            ["tasklist", "/FI", "PID eq %d" % again["pid"], "/NH"], capture_output=True, text=True
        ).stdout
        check("no process left behind after quit", str(again["pid"]) not in left, left)

# More output than the ring holds: the reader learns it lost some and goes on from what is kept
BURST = 'for i in range(4000):\n    print("line %05d " % i + "x" * 40)\nprint("burst done")\n'
with launch(BURST) as b:
    b.wait("burst done", timeout=20)
    r = b.console(since=0)
    check(
        "a reader behind the ring: lost, from the oldest kept byte",
        r["lost"] and r["start"] > 0,
        (r["lost"], r["start"]),
    )
    since, last = r["start"], ""
    for _ in range(10):
        r = b.console(since=since)
        if not r["data"]:
            break
        last, since = r["data"], r["next"]
    check(
        "following next reaches the end",
        "burst done" in last and "line 03999" in last,
        last[-200:],
    )

# Clients that misbehave: idle connections, cut bodies
with launch(LOOP) as b:
    b.wait("Hello")
    host, port = b.url.split("//")[1].rstrip("/").split(":")
    idle = [socket.create_connection((host, int(port))) for _ in range(9)]
    t = time.time()
    try:
        ok = b.request("GET", "/api/info", timeout=30)["board"] == "virtual"
    except Exception as e:
        ok = repr(e)
    check(
        "nine silent connections do not wedge the API",
        ok is True and time.time() - t < 20,
        (ok, time.time() - t),
    )
    for s in idle:
        s.close()
    for body in (b'{"code": "abc\\', b'{"code": "\\u12', b'{"code": "\\ud800\\u00"}'):
        c = http.client.HTTPConnection(host, int(port), timeout=10)
        c.request("POST", "/api/exec", body=body, headers={"Authorization": "Bearer " + b.token})
        status = c.getresponse().status
        c.close()
        check(
            "a cut JSON string: an answer, no crash %r" % body[-6:],
            status in (200, 400, 409),
            status,
        )
    check("the program still answers after them", b.info()["pid"] == b.info()["pid"])

# Listening beyond this computer needs a token chosen by the user
env = dict(HEADLESS)
env.pop("CIRCUITPY_API_TOKEN", None)
r = subprocess.run(
    [BIN, "--drive", drive('print("Hello")\n'), "--api", "0", "--api-host", "0.0.0.0"],
    env=env,
    cwd=T,
    stdin=subprocess.DEVNULL,
    capture_output=True,
    timeout=15,
)
err = r.stderr.decode(errors="replace")
check(
    "--api-host without CIRCUITPY_API_TOKEN: exit 2, says why",
    r.returncode == 2 and "CIRCUITPY_API_TOKEN" in err,
    (r.returncode, err),
)
print("DONE")
