"""The MCP server: python3 test_mcp.py /path/to/circuitpython

tools/cp_mcp.py started over pipes, as an agent's host starts it, and spoken to in JSON-RPC.
"""

import base64, json, os, subprocess, sys, threading, time
from harness import BIN, T, board_file, check, drive, png

# Beside the tests when they are copied out, else in the port's tools/
SERVER = os.path.join(T, "cp_mcp.py")
if not os.path.exists(SERVER):
    SERVER = os.path.join(T, "..", "tools", "cp_mcp.py")


class Client:
    """The host side: one request at a time, each answer read back by its id."""

    def __init__(self):
        self.p = subprocess.Popen(
            [sys.executable, SERVER, "--program", BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=T,
        )
        self.lines = []
        self.stderr = []
        threading.Thread(target=lambda: self.stderr.extend(self.p.stderr), daemon=True).start()
        self.n = 0

    def send(self, message):
        self.p.stdin.write(json.dumps(message).encode() + b"\n")
        self.p.stdin.flush()

    def read(self, timeout=90):
        """The next line on stdout, as bytes; None when the server ended or said nothing in time."""
        got = []
        t = threading.Thread(target=lambda: got.append(self.p.stdout.readline()), daemon=True)
        t.start()
        t.join(timeout)
        if got and got[0]:
            self.lines.append(got[0])
            return got[0]
        return None

    def request(self, method, params=None):
        self.n += 1
        self.send({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params or {}})
        line = self.read()
        if line is None:
            return {
                "error": {
                    "message": "no answer; stderr: "
                    + b"".join(self.stderr[-20:]).decode(errors="replace")
                }
            }
        return json.loads(line)

    def call(self, tool, **arguments):
        r = self.request("tools/call", {"name": tool, "arguments": arguments})
        return r.get("result") or {
            "isError": True,
            "content": [{"type": "text", "text": json.dumps(r)}],
        }


def text(result):
    return "".join(c.get("text", "") for c in result.get("content", []))


c = Client()
r = c.request(
    "initialize",
    {
        "protocolVersion": "2025-06-18",
        "capabilities": {},
        "clientInfo": {"name": "test", "version": "1"},
    },
)
res = r.get("result", {})
check(
    "initialize: the version asked for, tools, a name",
    res.get("protocolVersion") == "2025-06-18"
    and "tools" in res.get("capabilities", {})
    and res.get("serverInfo", {}).get("name"),
    r,
)
c.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
r = c.request("ping")
check(
    "a notification has no answer; ping answers {}",
    r.get("id") == c.n and r.get("result") == {},
    r,
)
check("stdout lines end in \\n alone", all(not x.endswith(b"\r\n") for x in c.lines), c.lines[-2:])

r = c.request("tools/list")
names = {t["name"] for t in r.get("result", {}).get("tools", [])}
want = {
    "launch",
    "attach",
    "stop",
    "info",
    "exec",
    "console",
    "write",
    "interrupt",
    "reload",
    "state",
    "panel",
    "display",
    "trace",
    "list_files",
    "read_file",
    "write_file",
}
check(
    "tools/list names every tool, each with a schema",
    want <= names and all("inputSchema" in t for t in r["result"]["tools"]),
    sorted(names),
)
r = c.request("nope/nothing")
check("an unknown method: -32601", r.get("error", {}).get("code") == -32601, r)

r = c.call("exec", code="1")
check(
    "a tool before launch: an error saying to launch", r.get("isError") and "launch" in text(r), r
)

BOARD = board_file('[i2c]\nTEMP = "mcp9808"\nOLED = "ssd1306 128x64"\n')
RED = """import board, displayio, time
bitmap = displayio.Bitmap(board.DISPLAY.width, board.DISPLAY.height, 1)
palette = displayio.Palette(1); palette[0] = 0xff0000
group = displayio.Group(); group.append(displayio.TileGrid(bitmap, pixel_shader=palette))
board.DISPLAY.root_group = group
print("Hello from code.py")
while True:
    time.sleep(0.01)
"""
D = drive(RED)
r = c.call("launch", drive=D, board_file=BOARD)
check("launch", not r.get("isError"), r)
r = c.call("console", wait="Hello from code.py", timeout=15)
check(
    "console waits for the text, escapes removed",
    not r.get("isError") and "Hello from code.py" in text(r) and "\x1b" not in text(r),
    r,
)
r = c.call("launch", drive=D)
check("a second launch: an error naming stop", r.get("isError") and "stop" in text(r), r)

shot = None
for _ in range(40):
    r = c.call("display")
    img = [x for x in r.get("content", []) if x.get("type") == "image"]
    if img and img[0].get("mimeType") == "image/png":
        data = base64.b64decode(img[0]["data"])
        w, h, px = png(data)
        shot = (w, h, px(w // 2, h // 2), len(data))
        if shot[2] == (255, 0, 0):
            break
    time.sleep(0.05)
check(
    "display: a PNG image content, 320x240, red, compressed",
    shot is not None and shot[:3] == (320, 240, (255, 0, 0)) and shot[3] < 320 * 240 * 3 // 2,
    shot,
)
r = c.call("display", name="OLED")
img = [x for x in r.get("content", []) if x.get("type") == "image"]
check(
    "a 128-pixel OLED is scaled up to 256",
    img and png(base64.b64decode(img[0]["data"]))[:2] == (256, 128),
    r,
)

r = c.call("panel", commands="set TEMP.temp 29.5")
check("panel set", not r.get("isError"), r)
r = c.call("state")
try:
    temp = [p for p in json.loads(text(r))["parts"] if p["name"] == "TEMP.temp"][0]["value"]
except (ValueError, KeyError, IndexError):
    temp = None
check("state: the value set, as JSON", temp is not None and abs(temp - 29.5) < 0.01, r)
r = c.call("panel", commands="press NOPE")
check("a panel line that fails is an error naming it", r.get("isError") and "NOPE" in text(r), r)

r = c.call("exec", code="6*7")
check(
    "exec while code.py runs: an error saying interrupt",
    r.get("isError") and "interrupt" in text(r),
    r,
)
r = c.call("exec", code="6*7", interrupt=True)
check("exec with interrupt: the value", not r.get("isError") and "42" in text(r), r)
r = c.call("exec", code="x = 5\nprint('x is', x)")
check("exec: what the code printed", not r.get("isError") and "x is 5" in text(r), r)
r = c.call("exec", code="y = 6\nprint('y set')\ny * 7")
check(
    "exec: statements, then the last expression's value",
    not r.get("isError") and "y set" in text(r) and "42" in text(r),
    r,
)
r = c.call("exec", code="for i in range(2):\n    print('i', i)")
check("exec: a block that ends in a statement", not r.get("isError") and "i 1" in text(r), r)
r = c.call("exec", code="1/0")
check(
    "exec that raises: an error with the traceback",
    r.get("isError") and "ZeroDivisionError" in text(r),
    r,
)

r = c.call("list_files")
check(
    "list_files: code.py with its size",
    ("code.py  %d" % os.path.getsize(os.path.join(D, "code.py"))) in text(r),
    r,
)
r = c.call("read_file", path="code.py")
check("read_file", "Hello from code.py" in text(r), r)
r = c.call("write_file", path="code.py", content='print("Second version")\n')
check("write_file", not r.get("isError"), r)
r = c.call("console", wait="Second version", timeout=15)
check(
    "writing code.py at the REPL runs it",
    "Second version" in text(r) and "did not appear" not in text(r),
    r,
)
check(
    "the wait after a write sees only the new run, not what came before",
    "Code done running" not in text(r) and "exec(" not in text(r),
    r,
)
c.call("exec", code="1", interrupt=True)
r = c.call("console", timeout=0)
check("exec leaves nothing of its own to read on the console", "exec(" not in text(r), r)
r = c.call("write_file", path="lib/mod/helper.py", content="X = 1\n")
check(
    "write_file makes the folders", os.path.exists(os.path.join(D, "lib", "mod", "helper.py")), r
)
r = c.call("list_files", path="lib")
check("list_files of a folder", "lib/mod/\nlib/mod/helper.py  6" in text(r), r)
for bad in ("../outside.txt", os.path.join(T, "outside.txt")):
    r = c.call("write_file", path=bad, content="no")
    check(
        "a path out of the drive is refused: " + bad,
        r.get("isError") and not os.path.exists(os.path.join(T, "outside.txt")),
        r,
    )
r = c.call("read_file", path="../test_mcp.py")
check("reading out of the drive is refused", r.get("isError"), r)

r = c.call("write", text="\x03")
check("write", not r.get("isError"), r)
r = c.call("trace")
check("trace answers", not r.get("isError"), r)
r = c.call("info")
check("info names the board", '"virtual"' in text(r), r)

r = c.call("stop")
check("stop", not r.get("isError"), r)
r = c.call("info")
check("after stop: an error saying to launch", r.get("isError") and "launch" in text(r), r)

r = c.call("launch", drive=drive('import board\nprint("dying")\nboard.exit(3)\n'))
time.sleep(1)
r = c.call("exec", code="1")
check("a program that ended: an error that says so", r.get("isError") and "ended" in text(r), r)
r = c.call("stop")
r = c.call("launch", drive=drive('print("again")\n'))
r2 = c.call("console", wait="again", timeout=15)
check("launch after a program ended", not r.get("isError") and "again" in text(r2), (r, r2))
r = c.call("stop")
r = c.call("launch", drive=drive('print("fast")\n'), fast_time=True)
check(
    "launch with fast_time: info says so",
    not r.get("isError") and '"fast_time": true' in text(r),
    r,
)

# Arguments that once ended the server: each is an error, and the server answers on
for tool, args in (
    ("write_file", {"path": "bad.txt", "content": "\ud800"}),
    ("write_file", {"path": "bad.txt", "content": 5}),
    ("write", {"text": "\ud800"}),
    ("console", {"wait": "\ud800", "timeout": 0}),
    ("read_file", {"path": "Z:/x" if os.name == "nt" else "/etc/passwd"}),
    ("stop", {}),
    ("attach", {"url": "localhost:8080", "token": "x"}),
    ("attach", {"url": "http://127.0.0.1/", "token": "x"}),
):
    r = c.call(tool, **args)
    if tool == "stop":
        continue
    alive_answer = c.request("ping")
    check(
        "%s %r: an error, the server goes on" % (tool, args),
        r.get("isError") and alive_answer.get("result") == {},
        (r, alive_answer),
    )
r = c.request("tools/call", ["not", "an", "object"])
check(
    "params that are not an object: an error answer",
    "error" in r and c.request("ping").get("result") == {},
    r,
)
c.call("launch", drive=drive('print("last")\n'))
pid = json.loads(text(c.call("info"))).get("pid")

c.p.stdin.close()
try:
    status = c.p.wait(timeout=15)
except subprocess.TimeoutExpired:
    c.p.kill()
    status = None
check("stdin closed ends the server", status == 0, status)


def alive(pid):
    if os.name == "nt":
        out = subprocess.run(
            ["tasklist", "/FI", "PID eq %d" % pid], capture_output=True, text=True
        ).stdout
        return str(pid) in out
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    if not os.path.exists("/proc"):  # macOS: ps names a zombie's state Z
        state = subprocess.run(
            ["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True
        ).stdout
        return not state.strip().startswith("Z")
    return not open("/proc/%d/stat" % pid).read().split(")")[-1].split()[0] == "Z"


end = time.time() + 5
while pid and alive(pid) and time.time() < end:
    time.sleep(0.1)
check("and the program it launched", pid and not alive(pid), pid)

# A server killed outright takes its board with it
c = Client()
c.request(
    "initialize",
    {
        "protocolVersion": "2025-06-18",
        "capabilities": {},
        "clientInfo": {"name": "t", "version": "1"},
    },
)
c.call("launch", drive=drive("import time\nwhile True:\n    time.sleep(0.01)\n"))
pid = json.loads(text(c.call("info"))).get("pid")
c.p.kill()
c.p.wait()
end = time.time() + 5
while pid and alive(pid) and time.time() < end:
    time.sleep(0.1)
check("a killed server's board ends too", pid and not alive(pid), pid)
if pid and alive(pid):
    subprocess.run(
        ["taskkill", "/F", "/PID", str(pid)] if os.name == "nt" else ["kill", "-9", str(pid)]
    )

# A program that never announces the API: launch gives up instead of hanging
if os.name == "nt":
    print("SKIP a program that never announces (needs a shell script)")
else:
    hang = os.path.join(T, "mcp_hang.sh")
    open(hang, "w").write("#!/bin/sh\nexec sleep 60\n")
    os.chmod(hang, 0o755)
    c = Client()
    c.request(
        "initialize",
        {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "t", "version": "1"},
        },
    )
    start = time.time()
    r = c.call("launch", drive=D, program=hang)
    check(
        "a program that never announces: launch fails in time",
        r.get("isError") and time.time() - start < 30,
        (r, time.time() - start),
    )
    c.p.stdin.close()
    c.p.wait(timeout=15)

# As in a release package: the server beside the program finds it without --program
import shutil, tempfile

package = tempfile.mkdtemp()
dlls = [
    os.path.join(os.path.dirname(BIN), f)
    for f in os.listdir(os.path.dirname(BIN))
    if f.endswith(".dll")
]
for f in [SERVER, os.path.join(os.path.dirname(os.path.abspath(SERVER)), "cpapi.py"), BIN] + dlls:
    shutil.copy(f, package)
p = subprocess.Popen(
    [sys.executable, os.path.join(package, "cp_mcp.py")],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
    cwd=package,
)
p.stdin.write(
    (
        json.dumps(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "tools/call",
                "params": {"name": "launch", "arguments": {"drive": drive('print("packaged")\n')}},
            }
        )
        + "\n"
    ).encode()
)
p.stdin.flush()
answer = p.stdout.readline()
check(
    "in a package the server launches the program beside it",
    b'"isError": false' in answer,
    answer[:300],
)
p.stdin.close()
p.wait(timeout=15)
shutil.rmtree(package, ignore_errors=True)

# The output up to a match, cut by byte offsets when the ring held a byte that is not UTF-8
sys.path.insert(0, os.path.dirname(os.path.abspath(SERVER)))
import cp_mcp


class Ring:
    """Stands for the program's answer: the ring holds b"\\xffab\\nHello rest", "Hello" matched."""

    process = None
    offset = trace_offset = 0

    def console(self, wait=None, timeout=0):
        return {
            "start": 0,
            "end": 14,
            "next": 9,
            "matched": True,
            "lost": False,
            "boot": 1,
            "data": "�ab\nHello rest",
        }


s = cp_mcp.Server("none")
s.board = Ring()
out = s.t_console(wait="Hello")
check(
    "the text up to a match is whole after an invalid byte",
    out.endswith("Hello") and s.board.offset == 9,
    repr(out),
)
