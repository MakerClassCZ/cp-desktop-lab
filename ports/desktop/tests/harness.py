"""What the tests share. A test is run as python3 test_X.py /path/to/circuitpython [...]: BIN is
that program, check() prints its PASS/FAIL line, and a program is started on a drive of its own
through the control API (tools/cpapi.py), headless, and driven and read with no sleeps.
"""
import os, shutil, subprocess, sys, tempfile
BIN = sys.argv[1] if len(sys.argv) > 1 else None
T = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [T, os.path.join(T, "..", "tools")]
import cpapi

HEADLESS = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
_files = tempfile.mkdtemp()     # board files and key scripts, outside any drive


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)[-500:]))


class Run:
    """The program as a process of its own, its console in a log beside the tests: for what the
    API does not show, a drive's first start, a reload after a host edit, a reset."""
    def __init__(self, args, cwd=T, env=HEADLESS, stdin=subprocess.DEVNULL):
        self.path = os.path.join(T, "out.txt")
        self.log = open(self.path, "wb")
        self.p = subprocess.Popen([BIN] + list(args), cwd=cwd, stdin=stdin, stdout=self.log,
                                  stderr=subprocess.STDOUT, env=env)

    def text(self):
        return open(self.path, "rb").read().decode(errors="replace")

    def wait_for(self, s, timeout=8, count=1):
        """Whether s has come count times."""
        import time
        end = time.time() + timeout
        while time.time() < end:
            if self.text().count(s) >= count:
                return True
            time.sleep(0.1)
        return False

    def stop(self):
        if self.p.poll() is None:
            if sys.platform == "win32":
                # A reset nests a child process: the tree by PID, never by name.
                subprocess.run(["taskkill", "/PID", str(self.p.pid), "/T", "/F"], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
            else:
                self.p.terminate()
        self.p.wait(timeout=5)
        self.log.close()


class Chip:
    """A board on a USB adapter (--board NAME): each code.py a process of its own on one drive.
    Ends the test with a SKIP when the program says the adapter is missing."""
    def __init__(self, board, missing):
        self.board = board
        self.D = drive('print("%s up")\n' % board)
        self.trace = os.path.join(T, "trace.txt")
        ok, text = self.run(None, board + " up")
        if not ok and any(m in text for m in missing):
            print("SKIP no %s:" % board, text.strip().splitlines()[-1] if text.strip() else "")
            sys.exit(0)
        check("program starts with --board " + board, ok, text)

    def run(self, src, expect, timeout=8, name=None, traced=False):
        """(whether expect came, the console); with traced, the trace lines too."""
        if src is not None:
            open(os.path.join(self.D, "code.py"), "w").write(src)
        if os.path.exists(self.trace):
            os.remove(self.trace)
        r = Run(["--board", self.board, "--drive", self.D] + (["--trace-bus", self.trace] if traced else []))
        ok = r.wait_for(expect, timeout)
        r.stop()
        if name:
            check(name, ok, r.text())
        if traced:
            lines = open(self.trace, encoding="utf8", errors="replace").read().splitlines() if os.path.exists(self.trace) else []
            return ok, r.text(), lines
        return ok, r.text()

    def after_reset(self, src, expect, name):
        """src runs twice, the second time after its microcontroller.reset(): "first" is True
        before it. The drive is writable from Python meanwhile."""
        open(os.path.join(self.D, "boot.py"), "w").write('import storage\nstorage.remount("/", readonly=False)\n')
        flag = os.path.join(self.D, "reset.flag")
        if os.path.exists(flag):
            os.remove(flag)
        self.run('import os\nfirst = "reset.flag" not in os.listdir("/")\nif first:\n    open("/reset.flag", "w").write("x")\n'
                 + src, expect, timeout=20, name=name)
        os.remove(os.path.join(self.D, "boot.py"))

    def second_refused(self, message):
        """While one program holds the adapter, a second exits 2 saying message."""
        open(os.path.join(self.D, "code.py"), "w").write('import time\nprint("holding")\nwhile True:\n    time.sleep(0.2)\n')
        holder = Run(["--board", self.board, "--drive", self.D])
        holder.wait_for("holding")
        second = subprocess.run([BIN, "--board", self.board, "--drive", self.D], cwd=T, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=HEADLESS, timeout=15)
        holder.stop()
        out = second.stdout.decode(errors="replace")
        check("second instance refused", second.returncode == 2 and message in out, "status %r %s" % (second.returncode, out))


def fetch_libs(lib, packages):
    """The libraries' .py sources into lib, from the bundle as pip packages them; downloaded once
    into libcache/ beside the tests."""
    import glob, zipfile
    cache = os.path.join(T, "libcache")
    missing = [p for p in packages if not glob.glob(os.path.join(cache, p.replace("-", "_") + "-*.whl"))]
    if missing:
        subprocess.run([sys.executable, "-m", "pip", "download", "-q", "--no-deps", "-d", cache] + missing, check=True)
    for p in packages:
        for whl in glob.glob(os.path.join(cache, p.replace("-", "_") + "-*.whl")):
            with zipfile.ZipFile(whl) as z:
                for n in z.namelist():
                    if n.endswith(".py") and n.startswith("adafruit_"):
                        z.extract(n, lib)


def drive(code, files=None, name="CIRCUITPY"):
    """A fresh drive beside the tests with code.py and files ({name: str or bytes})."""
    d = os.path.join(T, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    open(os.path.join(d, "code.py"), "w").write(code)
    for n, text in (files or {}).items():
        os.makedirs(os.path.dirname(os.path.join(d, n)), exist_ok=True)
        open(os.path.join(d, n), "wb" if isinstance(text, bytes) else "w").write(text)
    return d


def board_file(text, name="board.toml"):
    """A board file of text; None stays None."""
    if text is None:
        return None
    path = os.path.join(_files, name)
    open(path, "w").write(text)
    return path


def launch(code, board=None, files=None, args=(), **kw):
    """code.py started on a fresh drive with board (a board file's text): a cpapi.Board."""
    return cpapi.Board.launch(BIN, drive(code, files), board_file=board_file(board), args=args, **kw)


def seen(b, text, timeout=5):
    """Whether text comes on the console after what was read; reads past it."""
    try:
        b.wait(text, timeout=timeout)
        return True
    except TimeoutError:
        return False


def run(code, steps="", expect=None, board=None, files=None, args=(), timeout=15):
    """code.py run until expect shows on the console (or timeout), after the steps: one per line,
    "wait TEXT" for console text, "sleep MS", else a panel command (set, press, release, turn,
    send, down, up). Returns the console and the bus trace, all of each."""
    import time
    with launch(code, board, files, args) as b:
        for line in steps.splitlines():
            verb, _, rest = line.partition(" ")
            if verb == "wait":
                seen(b, rest, timeout)
            elif verb == "sleep":
                time.sleep(int(rest) / 1000)
            elif line.strip():
                b.panel(line)
        if expect is not None and seen(b, expect, timeout):
            seen(b, "\n", 2)            # the rest of its line
        return b.console(since=0)["data"], b.trace(since=0)["data"]


def ppm(path):
    """(width, height, pixel(x, y) -> (r, g, b)) of a binary PPM ("dump NAME PATH"), or None."""
    if not os.path.exists(path):
        return None
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = (int(v) for v in parts[1].split())
    body = parts[3]
    return w, h, lambda x, y: tuple(body[3 * (y * w + x):3 * (y * w + x) + 3])


def png(data):
    """(width, height, pixel(x, y) -> (r, g, b)) of the port's PNG: 8-bit RGB rows each after a
    filter byte of 0."""
    import struct, zlib
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    row = 1 + 3 * w
    return w, h, lambda x, y: tuple(raw[y * row + 1 + 3 * x:y * row + 4 + 3 * x])


def picture(b, name=None):
    """png() of a display through the API."""
    return png(b.display(name))


def shown(code, board=None, names=(None,), ready="drawn", timeout=15):
    """code.py run until it prints ready: (the console, a picture() of each display named)."""
    with launch(code, board) as b:
        seen(b, ready, timeout)
        return b.console(since=0)["data"], [picture(b, n) for n in names]


def refused(args=(), board=None, code='print("started")\n', timeout=10):
    """A start that should fail: (exit status, what it said), None as the status if it ran on."""
    command = [BIN, "--drive", drive(code)] + list(args)
    if board is not None:
        command += ["--board-file", board_file(board)]
    try:
        p = subprocess.run(command, capture_output=True, timeout=timeout, env=HEADLESS, cwd=T,
                           stdin=subprocess.DEVNULL)
        return p.returncode, (p.stdout + p.stderr).decode(errors="replace")
    except subprocess.TimeoutExpired as e:
        return None, ((e.stdout or b"") + (e.stderr or b"")).decode(errors="replace")
