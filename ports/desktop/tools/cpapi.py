"""A client for the CircuitPython program's control API (--api), standard library only.

with Board.launch("circuitpython", "CIRCUITPY") as b:
    b.wait("Hello")
    print(b.state())

From the command line, one run of a script: python3 cpapi.py run PROGRAM CODE (run --help).
"""

import http.client
import json
import os
import re
import secrets
import struct
import subprocess
import sys
import time
import zlib
from urllib.parse import urlencode

ANNOUNCE = re.compile(rb"circuitpython: api (http://[^/\s]+/) token ([0-9a-fA-F]+)")


class Picture:
    """A display's pixels: width, height and rgb, three bytes a pixel, rows from the top.
    p[x, y] is a pixel's colour as 0xRRGGBB."""

    def __init__(self, width, height, rgb):
        self.width, self.height, self.rgb = width, height, bytes(rgb)

    @classmethod
    def from_png(cls, data):
        """The program's PNG (/api/display): 8-bit RGB, each row after a filter byte of 0."""
        pos, idat, w, h = 8, b"", 0, 0
        while pos < len(data):
            n, kind = struct.unpack(">I4s", data[pos : pos + 8])
            body = data[pos + 8 : pos + 8 + n]
            if kind == b"IHDR":
                w, h = struct.unpack(">II", body[:8])
            elif kind == b"IDAT":
                idat += body
            pos += 12 + n
        raw = zlib.decompress(idat)
        row = 1 + 3 * w
        return cls(w, h, b"".join(raw[y * row + 1 : (y + 1) * row] for y in range(h)))

    def __getitem__(self, xy):
        i = 3 * (xy[1] * self.width + xy[0])
        return self.rgb[i] << 16 | self.rgb[i + 1] << 8 | self.rgb[i + 2]

    def count_colors(self):
        """{0xRRGGBB: how many pixels have it}."""
        counts = {}
        for i in range(0, len(self.rgb), 3):
            c = self.rgb[i] << 16 | self.rgb[i + 1] << 8 | self.rgb[i + 2]
            counts[c] = counts.get(c, 0) + 1
        return counts

    def diff(self, expected):
        """[(x, y, colour here, colour expected)] for each pixel that differs, row by row.
        expected is a Picture of the same size, or a function of (x, y) giving 0xRRGGBB, or None
        where any colour will do."""
        if isinstance(expected, Picture):
            if (expected.width, expected.height) != (self.width, self.height):
                raise ValueError(
                    "%dx%d against %dx%d"
                    % (self.width, self.height, expected.width, expected.height)
                )
            other = expected

            def expected(x, y):
                return other[x, y]

        out = []
        for y in range(self.height):
            for x in range(self.width):
                want = expected(x, y)
                have = self[x, y]
                if want is not None and want != have:
                    out.append((x, y, have, want))
        return out


def upto_next(r):
    """A stream answer's text up to its next offset. Cut from the end: a byte that was not UTF-8
    comes back as one 3-byte character, and those are rarer after the match than before it."""
    data = r["data"].encode()
    return data[: max(0, len(data) - (r["end"] - r["next"]))].decode(errors="replace")


# macOS has no PR_SET_PDEATHSIG: a watcher waits for this process or the program to end (kqueue)
# and ends the program in the first case. argv: this process's pid, the program's.
_WATCHER = """import os, select, signal, sys
parent, child = int(sys.argv[1]), int(sys.argv[2])
kq = select.kqueue()
ended = None
for pid in (child, parent):
    try:
        kq.control([select.kevent(pid, select.KQ_FILTER_PROC, select.KQ_EV_ADD, select.KQ_NOTE_EXIT)], 0)
    except ProcessLookupError:
        ended = pid
        break
if ended is None:
    ended = kq.control(None, 1)[0].ident
if ended == parent:
    try:
        os.kill(child, signal.SIGTERM)
    except ProcessLookupError:
        pass
"""


def _tie_to_this_process(p):
    """The program ends when this process ends, however it ends (a host killing an MCP server)."""
    if sys.platform == "darwin":
        subprocess.Popen(
            [sys.executable, "-c", _WATCHER, str(os.getpid()), str(p.pid)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        return
    if os.name != "nt":
        return
    import ctypes
    from ctypes import wintypes

    global _job
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    if _job is None:

        class Basic(ctypes.Structure):
            _fields_ = [
                ("a", ctypes.c_int64),
                ("b", ctypes.c_int64),
                ("LimitFlags", wintypes.DWORD),
                ("c", ctypes.c_size_t),
                ("d", ctypes.c_size_t),
                ("e", wintypes.DWORD),
                ("f", ctypes.c_size_t),
                ("g", wintypes.DWORD),
                ("h", wintypes.DWORD),
            ]

        class Extended(ctypes.Structure):
            _fields_ = [("basic", Basic), ("io", ctypes.c_uint64 * 6), ("m", ctypes.c_size_t * 4)]

        k.CreateJobObjectW.restype = wintypes.HANDLE
        job = k.CreateJobObjectW(None, None)
        info = Extended()
        info.basic.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not job or not k.SetInformationJobObject(
            wintypes.HANDLE(job), 9, ctypes.byref(info), ctypes.sizeof(info)
        ):
            return
        _job = job
    # A reset's child started later is in the job too.
    k.AssignProcessToJobObject(wintypes.HANDLE(_job), wintypes.HANDLE(int(p._handle)))


_job = None


def _die_with_parent():
    """Linux: SIGTERM when the process that started the program ends (macOS: _WATCHER)."""
    try:
        import ctypes
        import signal

        ctypes.CDLL(None).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    except (OSError, AttributeError):
        pass


class ApiError(Exception):
    """The API answered with an error status."""

    def __init__(self, status, message):
        super().__init__("%d %s" % (status, message))
        self.status = status
        self.message = message


class CircuitPythonError(Exception):
    """Code run through exec raised (the traceback) or ran out of time ("timeout")."""

    def __init__(self, error, output=""):
        super().__init__(error)
        self.error = error
        self.output = output


class Board:
    """A running program, reached through its API."""

    def __init__(self, url, token, process=None):
        self.url = url
        self.token = token
        self.process = process
        self.offset = 0  # the console read so far
        self.trace_offset = 0  # and the trace
        self.boot = None  # the process they belong to; a reset starts them over
        self.retry = 10  # seconds a refused connection is tried again
        hostport = url.split("//", 1)[1].rstrip("/")
        self._host, port = hostport.rsplit(":", 1)
        self._port = int(port)

    @classmethod
    def launch(
        cls,
        program,
        drive,
        board_file=None,
        board=None,
        headless=True,
        args=(),
        timeout=15,
        env=None,
        fast_time=False,
    ):
        """Start program on drive with --api 0 and a token of its own; stderr is read for the port.
        fast_time: its sleeps return at once and move its clock on (--fast-time)."""
        token = secrets.token_hex(16)
        e = dict(os.environ if env is None else env, CIRCUITPY_API_TOKEN=token)
        if headless:
            e.setdefault("SDL_VIDEODRIVER", "dummy")
            e.setdefault("SDL_AUDIODRIVER", "dummy")
        # Paths from here: the program runs in the drive's parent directory. A bare name is PATH's.
        if os.path.dirname(program):
            program = os.path.abspath(program)
        drive = os.path.abspath(drive)
        command = [program, "--drive", drive, "--api", "0"]
        if board_file:
            command += ["--board-file", os.path.abspath(board_file)]
        if board:
            command += ["--board", board]
        if fast_time:
            command += ["--fast-time"]
        command += list(args)
        p = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            env=e,
            cwd=os.path.dirname(os.path.abspath(drive)),
            preexec_fn=_die_with_parent if os.name != "nt" else None,
        )
        _tie_to_this_process(p)
        # stderr is read on a thread from the start: the announcement within timeout even when the
        # program prints nothing, and afterwards so the program never blocks writing to it.
        import threading

        lines, found = [], []
        announced = threading.Event()

        def drain():
            for line in p.stderr:
                lines.append(line.decode(errors="replace"))
                m = ANNOUNCE.search(line)
                if m and not found:
                    found.append(m)
                    announced.set()
            announced.set()

        threading.Thread(target=drain, daemon=True).start()
        announced.wait(timeout)
        if not found:
            p.kill()
            p.wait()
            raise RuntimeError("no api announcement from %s: %s" % (program, "".join(lines)))
        b = cls(found[0].group(1).decode(), found[0].group(2).decode(), p)
        b.stderr = lines
        return b

    @classmethod
    def attach(cls, url, token):
        return cls(url, token)

    def request(self, method, path, body=None, timeout=30, raw=False):
        """One request; JSON in and out unless raw. Raises ApiError on an error status."""
        headers = {"Authorization": "Bearer " + self.token}
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        elif isinstance(body, str):
            body = body.encode()
        # A reset restarts the program on the same port: a refused connection is tried again, and a
        # GET the old program cut off on its way out (a POST could have been done once already).
        end = time.time() + self.retry
        while True:
            c = http.client.HTTPConnection(self._host, self._port, timeout=timeout)
            try:
                c.request(method, path, body=body, headers=headers)
                r = c.getresponse()
                data = r.read()
                break
            except (
                ConnectionRefusedError,
                ConnectionResetError,
                http.client.RemoteDisconnected,
            ) as e:
                cut = not isinstance(e, ConnectionRefusedError)
                if (
                    (cut and method != "GET")
                    or time.time() > end
                    or (self.process is not None and self.process.poll() is not None)
                ):
                    raise
                time.sleep(0.1)
            finally:
                c.close()
        if r.status >= 400:
            try:
                answer = json.loads(data)
                # A panel answer names the lines that failed.
                message = answer.get("error") or "; ".join(
                    "%s: %s" % (x["line"], x["error"])
                    for x in answer.get("results", [])
                    if not x["ok"]
                )
            except ValueError:
                message = data.decode(errors="replace")
            raise ApiError(r.status, message)
        return data if raw else json.loads(data or b"null")

    def info(self):
        return self.request("GET", "/api/info")

    # ---- the console ----

    def console(self, since=None, wait=None, timeout=0):
        """The console's output from since (default: where this client last read), as the server
        returns it: start, end, next, lost, matched, data. Does not move the client's offset."""
        return self._stream("/api/console", "offset", since, wait, timeout)

    def _stream(self, path, attr, since, wait, timeout):
        q = {
            "since": getattr(self, attr) if since is None else since,
            "timeout": int(timeout * 1000),
        }
        if wait is not None:
            q["wait"] = wait
        if since is None and self.boot is not None:
            q["boot"] = self.boot  # the server reads from 0 when a reset has started a new run
        r = self.request("GET", path + "?" + urlencode(q), timeout=timeout + 30)
        if since is None and self.boot is not None and r["boot"] != self.boot:
            # A new process after a reset: its streams start from 0.
            self.offset = self.trace_offset = 0
            self.boot = r["boot"]
            return self._stream(path, attr, since, wait, timeout)
        self.boot = r["boot"]
        return r

    def wait(self, text, timeout=10):
        """Waits for text on the console after the last read; returns the output up to the match
        and moves past it. TimeoutError, with what came, when it does not appear."""
        r = self.console(wait=text, timeout=timeout)
        if not r["matched"]:
            raise TimeoutError(
                "%r not seen in %s s; the console: %r" % (text, timeout, r["data"][-800:])
            )
        self.offset = r["next"]
        return upto_next(r)

    def read(self, timeout=0):
        """What came since the last read (waiting up to timeout for something), moving past it."""
        r = self.console(timeout=timeout)
        self.offset = r["next"]
        return r["data"]

    # ---- the bus trace ----

    def trace(self, since=None, wait=None, timeout=0):
        """The bus trace's lines from since (default: where this client last read), as console()."""
        return self._stream("/api/trace", "trace_offset", since, wait, timeout)

    def trace_wait(self, text, timeout=10):
        """Waits for text in the trace after the last read; returns the lines up to it."""
        r = self.trace(wait=text, timeout=timeout)
        if not r["matched"]:
            raise TimeoutError(
                "%r not in the trace in %s s: %r" % (text, timeout, r["data"][-800:])
            )
        self.trace_offset = r["next"]
        return upto_next(r)

    def write(self, data):
        """Console input, as typed."""
        return self.request("POST", "/api/console", body=data)

    # ---- the REPL ----

    def run(self, code, interrupt=False, timeout=10):
        """The exec answer as the server gives it: ok, output, value, error."""
        return self.request(
            "POST",
            "/api/exec",
            body={"code": code, "interrupt": interrupt, "timeout_ms": int(timeout * 1000)},
            timeout=timeout + 30,
        )

    def exec(self, code, interrupt=False, timeout=10):
        """Runs code in the REPL; returns what it printed. CircuitPythonError when it raised or
        ran out of time. interrupt: stop a running code.py first (else ApiError 409)."""
        r = self.run(code, interrupt, timeout)
        if not r["ok"]:
            raise CircuitPythonError(r["error"], r["output"])
        return r["output"]

    def eval(self, expr, interrupt=False, timeout=10):
        """The repr of an expression's value, as the REPL would print it (None: no value)."""
        r = self.run(expr, interrupt, timeout)
        if not r["ok"]:
            raise CircuitPythonError(r["error"], r["output"])
        return r["value"]

    # ---- the board panel ----

    def panel(self, *lines):
        """Key script lines (set, press, release, turn, send, down, up, close), applied in order before
        this returns. ApiError naming the first line that failed."""
        return self.request("POST", "/api/panel", body="\n".join(lines))["results"]

    def state(self):
        """The parts, NeoPixels, displays and gamepad buttons, as the panel shows them."""
        return self.request("GET", "/api/state")

    def display(self, name=None):
        """A display's picture as PNG bytes: board.DISPLAY, or a board file's display by name."""
        return self.request(
            "GET", "/api/display" + ("?" + urlencode({"name": name}) if name else ""), raw=True
        )

    def pixels(self, name=None):
        """A display's picture as a Picture: its pixels, with count_colors() and diff()."""
        return Picture.from_png(self.display(name))

    def part(self, name):
        for p in self.state()["parts"]:
            if p["name"] == name:
                return p
        raise KeyError(name)

    def set(self, name, value):
        self.panel("set %s %s" % (name, value))

    def press(self, name):
        self.panel("press " + name)

    def release(self, name):
        self.panel("release " + name)

    def turn(self, name, detents):
        self.panel("turn %s %d" % (name, detents))

    def interrupt(self):
        self.request("POST", "/api/interrupt")

    def reload(self):
        self.request("POST", "/api/reload")

    def quit(self):
        try:
            self.request("POST", "/api/quit")
        except (ConnectionError, http.client.HTTPException, OSError):
            pass

    def close(self):
        """Quit; a program that does not end in 5 s is killed."""
        if self.process is None or self.process.poll() is not None:
            return
        self.quit()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# The terminal's escapes (the window title, line erases), left out of what run prints.
_ESCAPES = re.compile(r"\x1b\][^\x1b\x07]*(?:\x1b\\|\x07)|\x1b\[[0-9;?]*[A-Za-z]")


def _until(b, texts, timeout):
    """(the console read on from where b was, the first of texts in it or None) within timeout.
    Read as it comes, so the text before a hard fault's reset is kept."""
    out, end = "", time.time() + timeout
    while True:
        r = b.console(timeout=0.1)
        b.offset = r["next"]
        out += r["data"]
        seen = next((t for t in texts if t in out), None)
        if seen or time.time() > end or (b.process is not None and b.process.poll() is not None):
            return out, seen


def run(argv):
    """cpapi.py run PROGRAM CODE: CODE (a .py file as code.py, or a directory as the drive) run
    headless until code.py ends or --wait's text comes; its console printed, and the display if
    asked. Exit status 0, 1 when it did not end in time, 2 after a hard fault."""
    import argparse
    import shutil
    import tempfile

    ap = argparse.ArgumentParser(prog="cpapi.py run", description=run.__doc__)
    ap.add_argument("program")
    ap.add_argument("code")
    ap.add_argument("--board-file")
    ap.add_argument("--board")
    ap.add_argument("--wait", help="stop at this console text, not when code.py ends")
    ap.add_argument("--timeout", type=float, default=30, help="seconds for each run (default 30)")
    ap.add_argument("--runs", type=int, default=1, help="run code.py this many times (Ctrl-D)")
    ap.add_argument("--display", help="the display to read: DISPLAY (default) or a board file's")
    ap.add_argument("--png", help="save the display as this PNG after the last run")
    ap.add_argument("--colors", action="store_true", help="print the display's pixels per colour")
    ap.add_argument("--fast-time", action="store_true")
    a = ap.parse_args(argv)

    drive = os.path.join(tempfile.mkdtemp(), "CIRCUITPY")
    if os.path.isdir(a.code):
        shutil.copytree(a.code, drive)
    else:
        os.makedirs(drive)
        shutil.copy(a.code, os.path.join(drive, "code.py"))
    texts = [a.wait] if a.wait else ["Code done running."]
    texts.append("You are in safe mode")
    status = 0
    with Board.launch(
        a.program, drive, board_file=a.board_file, board=a.board, fast_time=a.fast_time
    ) as b:
        for i in range(a.runs):
            if i:
                b.reload()
            out, seen = _until(b, texts, a.timeout)
            print("--- run %d" % (i + 1))
            print(_ESCAPES.sub("", out).replace("\r\n", "\n").rstrip())
            if seen == "You are in safe mode":
                status = 2
                break
            if seen is None:
                print("--- not ended in %g s" % a.timeout)
                status = 1
                break
        if status != 2 and (a.png or a.colors):
            data = b.display(a.display)
            if a.png:
                open(a.png, "wb").write(data)
                print("--- display saved:", a.png)
            if a.colors:
                p = Picture.from_png(data)
                print("--- %dx%d, pixels per colour" % (p.width, p.height))
                for c, n in sorted(p.count_colors().items(), key=lambda x: -x[1]):
                    print("0x%06x %d" % (c, n))
        # The backtrace comes on stderr, read on a thread, just before the safe mode's console text.
        end = time.time() + (2 if status == 2 else 0)
        while time.time() < end and not any("hard fault" in x for x in b.stderr):
            time.sleep(0.05)
        fault = [x for x in b.stderr if x.strip() and not ANNOUNCE.search(x.encode())]
        fault = fault[next((i for i, x in enumerate(fault) if "hard fault" in x), len(fault)) :]
        if fault:
            print("--- " + "".join(fault).strip())
            status = 2
    return status


if __name__ == "__main__":
    if sys.argv[1:2] != ["run"]:
        print("usage: python cpapi.py run PROGRAM CODE [options]  (run --help)", file=sys.stderr)
        sys.exit(2)
    sys.exit(run(sys.argv[2:]))
