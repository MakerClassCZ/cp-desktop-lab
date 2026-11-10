"""A client for the CircuitPython program's control API (--api), standard library only.

with Board.launch("circuitpython", "CIRCUITPY") as b:
    b.wait("Hello")
    print(b.state())

From the command line, one run of a script: python3 cpapi.py run PROGRAM CODE (run --help);
programs started by hand on one Bluetooth air: python3 cpapi.py air PORT PORT... (air --help).
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
from collections import Counter
from urllib.parse import urlencode

ANNOUNCE = re.compile(rb"circuitpython: api (http://[^/\s]+/) token ([0-9a-fA-F]+)")


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# Terminal escapes: OSC (the title bar) up to BEL or ST, CSI, and the two-byte ones.
ESCAPES = re.compile(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b\[[0-?]*[ -/]*[@-~]|\x1b[@-Z\\-_]")


def plain(text):
    """Console text as a reader wants it: no escapes, \\n line ends."""
    return ESCAPES.sub("", text).replace("\r\n", "\n").replace("\r", "")


def png_parts(data):
    """A PNG's IHDR and its image data inflated: (ihdr bytes, raw rows)."""
    if data[:8] != PNG_SIGNATURE:
        raise ValueError("not a PNG")
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos : pos + 8])
        if kind == b"IHDR":
            ihdr = data[pos + 8 : pos + 8 + n]
        elif kind == b"IDAT":
            idat += data[pos + 8 : pos + 8 + n]
        pos += 12 + n
    return ihdr, zlib.decompress(idat)


class Picture:
    """A display's pixels: width, height and rgb, three bytes a pixel, rows from the top.
    p[x, y] is a pixel's colour as 0xRRGGBB."""

    def __init__(self, width, height, rgb):
        self.width, self.height, self.rgb = width, height, bytes(rgb)

    @classmethod
    def from_png(cls, data):
        """The program's PNG (/api/display): 8-bit RGB, each row after a filter byte of 0."""
        ihdr, raw = png_parts(data)
        w, h, depth, color = struct.unpack(">IIBB", ihdr[:10])
        row = 1 + 3 * w
        if (depth, color) != (8, 2) or any(raw[y * row] for y in range(h)):
            raise ValueError("not the port's PNG: 8-bit RGB, unfiltered")
        return cls(w, h, b"".join(raw[y * row + 1 : (y + 1) * row] for y in range(h)))

    def __getitem__(self, xy):
        i = 3 * (xy[1] * self.width + xy[0])
        return self.rgb[i] << 16 | self.rgb[i + 1] << 8 | self.rgb[i + 2]

    def count_colors(self):
        """{0xRRGGBB: how many pixels have it}."""
        rgb = self.rgb
        return dict(Counter(int.from_bytes(rgb[i : i + 3], "big") for i in range(0, len(rgb), 3)))

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


class Recording:
    """What one audio output played (/api/audio): pin, rate, channels, samples (an array of
    signed 16-bit values, channels interleaved), underruns (times it ran dry with more to play,
    in real time) and dropped (frames past the recording's limit). Times are in seconds."""

    def __init__(self, meta, wav):
        import array

        self.pin, self.rate, self.channels = meta["pin"], meta["rate"], meta["channels"]
        self.underruns, self.dropped = meta["underruns"], meta["dropped"]
        self.wav = wav
        self.samples = array.array("h", wav[44:])
        if sys.byteorder != "little":
            self.samples.byteswap()

    @property
    def seconds(self):
        return len(self.samples) / self.channels / self.rate

    def channel(self, n=0, start=0, end=None):
        """One channel's samples from start to end."""
        a = int(start * self.rate)
        b = len(self.samples) // self.channels if end is None else int(end * self.rate)
        return self.samples[a * self.channels + n : b * self.channels : self.channels]

    def peak(self, start=0, end=None, n=0):
        """The largest level, 0 to 1."""
        s = self.channel(n, start, end)
        return max((abs(x) for x in s), default=0) / 32768

    def rms(self, start=0, end=None, n=0):
        """The root mean square level, 0 to 1."""
        s = self.channel(n, start, end)
        return (sum(x * x for x in s) / len(s)) ** 0.5 / 32768 if s else 0.0

    def frequency(self, start=0, end=None, n=0):
        """The tone's frequency in Hz: the median period between its rising zero crossings, so
        silences and other notes around it do not count; None in silence."""
        s = self.channel(n, start, end)
        level = max(64, int(self.peak(start, end, n) * 32768 * 0.1))
        low, last, periods = False, None, []
        for i, x in enumerate(s):
            if x < -level:
                low = True
            elif x > level and low:
                low = False
                if last is not None:
                    periods.append(i - last)
                last = i
        if not periods:
            return None
        periods.sort()
        return self.rate / periods[len(periods) // 2]

    def silences(self, min_ms=20, level=0.01, n=0):
        """[(start, end)] of each stretch at least min_ms long below level: gaps and drop-outs."""
        s = self.channel(n)
        limit, need = level * 32768, int(self.rate * min_ms / 1000)
        out, run = [], None
        for i, x in enumerate(list(s) + [32767]):
            if abs(x) < limit:
                run = i if run is None else run
            elif run is not None:
                if i - run >= need:
                    out.append((run / self.rate, i / self.rate))
                run = None
        return out

    def save(self, path):
        open(path, "wb").write(self.wav)

    def summary(self):
        f = self.frequency()
        return (
            "%s: %d Hz, %d channel%s, %.2f s, peak %.2f, rms %.2f%s, %d silences, %d underruns"
            % (
                self.pin,
                self.rate,
                self.channels,
                "" if self.channels == 1 else "s",
                self.seconds,
                self.peak(),
                self.rms(),
                ", about %.0f Hz" % f if f else "",
                len(self.silences()),
                self.underruns,
            )
        )


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


def _board_file(name, program):
    """A board file's path: as given when it is there, else a bare name ("feather.toml") from the
    boards/files beside the program or this module, in a release package and in the port alike."""
    if os.path.exists(name) or os.path.dirname(name):
        if not os.path.exists(name):
            raise FileNotFoundError("no board file %s" % name)
        return os.path.abspath(name)
    here = os.path.dirname(os.path.abspath(__file__))
    tried = []
    for base in (os.path.dirname(os.path.abspath(program)), here):
        for files in (
            os.path.join(base, "boards", "files"),
            os.path.join(base, "..", "boards", "files"),
        ):
            path = os.path.normpath(os.path.join(files, name))
            if os.path.exists(path):
                return path
            tried.append(path)
    raise FileNotFoundError("no board file %s (nor %s)" % (name, ", ".join(tried)))


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
        code_file=None,
        audio_capture=False,
    ):
        """Start program on drive with --api 0 and a token of its own; stderr is read for the port.
        fast_time: its sleeps return at once and move its clock on (--fast-time). code_file: a
        file on the drive run in place of code.py (--code). audio_capture: record what the audio
        outputs play from the start, for audio() (--audio-capture)."""
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
            command += ["--board-file", _board_file(board_file, program)]
        if board:
            command += ["--board", board]
        if fast_time:
            command += ["--fast-time"]
        if code_file:
            command += ["--code", code_file]
        if audio_capture:
            command += ["--audio-capture"]
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

    def follow(self, stream="console", wait=None, timeout=0):
        """The console's (or the trace's) text since the last read, moving past it: (text,
        matched, lost). With wait, up to that text, waiting up to timeout for it; without, up to
        timeout for anything. lost: some of it was gone from the program's buffer."""
        attr = "offset" if stream == "console" else "trace_offset"
        r = self._stream("/api/" + stream, attr, None, wait, timeout)
        setattr(self, attr, r["next"])
        return (upto_next(r) if r["matched"] else r["data"]), r["matched"], r["lost"]

    def skip(self):
        """What the console holds now is not to be read: the next read starts after it."""
        self.offset = self.console(timeout=0)["end"]

    def wait(self, text, timeout=10):
        """Waits for text on the console after the last read; returns the output up to the match
        and moves past it. TimeoutError, with what came, when it does not appear."""
        got, matched, _ = self.follow(wait=text, timeout=timeout)
        if not matched:
            raise TimeoutError(
                "%r not seen in %s s; the console: %r" % (text, timeout, got[-800:])
            )
        return got

    def read(self, timeout=0):
        """What came since the last read (waiting up to timeout for something), moving past it."""
        return self.follow(timeout=timeout)[0]

    # ---- the bus trace ----

    def trace(self, since=None, wait=None, timeout=0):
        """The bus trace's lines from since (default: where this client last read), as console()."""
        return self._stream("/api/trace", "trace_offset", since, wait, timeout)

    def trace_wait(self, text, timeout=10):
        """Waits for text in the trace after the last read; returns the lines up to it."""
        got, matched, _ = self.follow("trace", wait=text, timeout=timeout)
        if not matched:
            raise TimeoutError("%r not in the trace in %s s: %r" % (text, timeout, got[-800:]))
        return got

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
        """The repr of an expression's value, or of the last line's after statements, as the REPL would
        print it (None: no value)."""
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

    def touch(self, x, y, display="DISPLAY"):
        """A touch at x, y of the display (down, or moved there while down)."""
        self.panel("touch %s %d %d" % (display, x, y))

    def untouch(self, display="DISPLAY"):
        self.panel("untouch " + display)

    def tap(self, x, y, display="DISPLAY", hold=0.15):
        """Down at x, y, held for hold seconds (a program polls), then lifted."""
        import time

        self.touch(x, y, display)
        time.sleep(hold)
        self.untouch(display)

    def turn(self, name, detents):
        self.panel("turn %s %d" % (name, detents))

    def audio_start(self):
        """Clear the recordings and record what the audio outputs play from now on (from the start
        with launch(audio_capture=True))."""
        return self.request("POST", "/api/audio/start")

    def audio_stop(self):
        return self.request("POST", "/api/audio/stop")

    def audio(self):
        """[Recording] of what each audio output played since recording started, one per output
        pin, rate and channel count."""
        listed = self.request("GET", "/api/audio")["recordings"]
        return [
            Recording(m, self.request("GET", "/api/audio/wav?index=%d" % m["index"], raw=True))
            for m in listed
        ]

    def after_run(self, timeout=5):
        """What the last run left behind, once its VM has ended (waited for up to timeout s):
        leaked_pins (claimed by objects that did not release them; a board keeps them claimed
        too), outside_heap and outside_heap_change (bytes allocated outside the VM's heap, and the
        change since the VM before), runs (the VMs ended so far: boot.py, code.py and the REPL each
        count), result (what the program reported with lab.result: {"status", "detail"}, or
        None), faults_fired ([{"point", "action", "times"}]) and vm_running (true when it did not
        end in time)."""
        end = time.time() + timeout
        while True:
            r = self.request("GET", "/api/after-run")
            if not r["vm_running"] or time.time() > end:
                return r
            time.sleep(0.05)

    def result(self, timeout=5):
        """What the last run reported with lab.result: {"status": "pass", "fail" or "skip",
        "detail"}, or None. A run that ended in an exception has failed whatever it reported."""
        return self.after_run(timeout)["result"]

    def faults(self):
        """The fault rules now ("rule", "inside": set by the run, "hits", "fired") and the faults
        fired in this run. Rules come as panel lines: b.panel("fault sd.write error once")."""
        return self.request("GET", "/api/faults")

    def fired(self):
        """The faults fired in this run: [{"point", "action", "times"}]."""
        return self.faults()["fired"]

    def interrupt(self):
        self.request("POST", "/api/interrupt")

    def reload(self):
        self.request("POST", "/api/reload")

    def quit(self):
        try:
            self.request("POST", "/api/quit")
        except (ConnectionError, http.client.HTTPException, OSError):
            pass

    @property
    def ble(self):
        """The phone of the virtual Bluetooth controller: scan(), connect() (a Ble)."""
        return Ble(self)

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


# ---- Bluetooth LE: the phone of the virtual controller ----

BASE_UUID = "-0000-1000-8000-00805f9b34fb"


def uuid(u):
    """A UUID as its full lowercase text: 0x2A00, "2a00", "0x2a00" or a 128-bit one."""
    if isinstance(u, int):
        return "%08x%s" % (u, BASE_UUID)
    u = u.lower()
    if u.startswith("0x"):
        u = u[2:]
    if len(u) <= 8 and "-" not in u:
        return "%08x%s" % (int(u, 16), BASE_UUID)
    h = u.replace("-", "")
    return "%s-%s-%s-%s-%s" % (h[:8], h[8:12], h[12:16], h[16:20], h[20:])


def _uuid_from_le(data):
    """A UUID from ATT's little-endian bytes (2 or 16 of them)."""
    if len(data) == 2:
        return uuid(data[0] | data[1] << 8)
    h = bytes(reversed(data)).hex()
    return "%s-%s-%s-%s-%s" % (h[:8], h[8:12], h[12:16], h[16:20], h[20:])


class Advertisement:
    """What the program advertises: name, services (UUIDs), manufacturer data, raw bytes."""

    def __init__(self, state):
        self.raw = bytes.fromhex(state.get("data", "")) + bytes.fromhex(
            state.get("scan_response", "")
        )
        self.connectable = state.get("connectable", False)
        self.name, self.services, self.manufacturer = None, [], b""
        i = 0
        while i + 1 < len(self.raw) and self.raw[i]:
            length, kind = self.raw[i], self.raw[i + 1]
            value = self.raw[i + 2 : i + 1 + length]
            if kind in (0x08, 0x09) and (self.name is None or kind == 0x09):
                self.name = value.decode(errors="replace")
            elif kind in (0x02, 0x03):
                self.services += [
                    uuid(value[k] | value[k + 1] << 8) for k in range(0, len(value) - 1, 2)
                ]
            elif kind in (0x06, 0x07):
                self.services += [
                    _uuid_from_le(value[k : k + 16]) for k in range(0, len(value) - 15, 16)
                ]
            elif kind == 0xFF:
                self.manufacturer = value
            i += 1 + length

    def __repr__(self):
        return "Advertisement(name=%r, services=%r)" % (self.name, self.services)


# A characteristic's property bits, named as _bleio.Characteristic's constants are.
PROPERTIES = (
    (0x01, "broadcast"),
    (0x02, "read"),
    (0x04, "write_no_response"),
    (0x08, "write"),
    (0x10, "notify"),
    (0x20, "indicate"),
    (0x40, "signed_write"),
    (0x80, "extended"),
)


class Characteristic:
    def __init__(self, uuid, handle, properties):
        self.uuid, self.handle, self.properties = uuid, handle, properties
        self.cccd = None  # the handle of its Client Characteristic Configuration, if it has one
        self.subscribed = False

    def names(self):
        """Its properties by name, in the order of their bits: ["read", "write", "notify"]."""
        return [name for bit, name in PROPERTIES if self.properties & bit]

    def __repr__(self):
        return "Characteristic(%s, 0x%04x)" % (self.uuid, self.handle)


class Service:
    def __init__(self, uuid, start, end):
        self.uuid, self.start, self.end, self.characteristics = uuid, start, end, []

    def __repr__(self):
        return "Service(%s, 0x%04x-0x%04x)" % (self.uuid, self.start, self.end)


# The ATT error codes (Bluetooth Core, Vol 3, Part F, 3.4.1.1).
ATT_ERRORS = {
    0x01: "invalid handle",
    0x02: "read not permitted",
    0x03: "write not permitted",
    0x04: "invalid PDU",
    0x05: "insufficient authentication",
    0x06: "request not supported",
    0x07: "invalid offset",
    0x08: "insufficient authorization",
    0x09: "prepare queue full",
    0x0A: "attribute not found",
    0x0B: "attribute not long",
    0x0C: "insufficient encryption key size",
    0x0D: "invalid attribute value length",
    0x0E: "unlikely error",
    0x0F: "insufficient encryption",
    0x10: "unsupported group type",
    0x11: "insufficient resources",
}


class AttError(Exception):
    """An ATT Error Response: the request's opcode, the handle, the error code."""

    def __init__(self, opcode, handle, code):
        super().__init__(
            "ATT error 0x%02x (%s) on 0x%02x at handle 0x%04x"
            % (code, ATT_ERRORS.get(code, "unknown"), opcode, handle)
        )
        self.opcode, self.handle, self.code = opcode, handle, code


NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # the phone writes here
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # the program notifies here


# The BLE workflow (supervisor/shared/bluetooth/): the file transfer service and CircuitPython's
# serial service, as code.circuitpython.org and Glider use them.
FT_SERVICE = 0xFEBB
FT_VERSION = "adaf0100-4669-6c65-5472-616e73666572"
FT_TRANSFER = "adaf0200-4669-6c65-5472-616e73666572"
SERIAL_SERVICE = "adaf0001-4369-7263-7569-74507974686e"
SERIAL_RX = "adaf0002-4369-7263-7569-74507974686e"  # the phone writes here
SERIAL_TX = "adaf0003-4369-7263-7569-74507974686e"  # the board notifies here
SERIAL_VERSION = "adaf0100-4369-7263-7569-74507974686e"

# The file transfer protocol's statuses (file_transfer_protocol.h).
FT_STATUS = {
    0x01: "ok",
    0x02: "error",
    0x03: "error_no_file",
    0x04: "error_protocol",
    0x05: "error_readonly",
}


class FileTransferError(Exception):
    """A status other than ok: status is its name ("error_no_file"), command the answer's."""

    def __init__(self, command, status):
        self.status = FT_STATUS.get(status, "0x%02x" % status)
        self.command = command
        super().__init__("file transfer 0x%02x: %s" % (command, self.status))


class FileEntry:
    """One entry of listdir(): its name, whether it is a directory, size, mtime (ns since 1970)."""

    def __init__(self, name, directory, size, mtime):
        self.name, self.directory, self.size, self.mtime = name, directory, size, mtime

    def __repr__(self):
        return "FileEntry(%r, %d)" % (self.name + ("/" if self.directory else ""), self.size)


class FileTransfer:
    """The BLE workflow's file transfer protocol, written from file_transfer_protocol.h: each
    command goes out as Write Commands to the transfer characteristic in pieces of MTU-3 bytes,
    the answers come back as its notifications and are put together by the protocol's struct
    sizes. A status other than ok raises FileTransferError."""

    CHUNK = 512  # the most a read asks for at a time, as the editors do

    def __init__(self, peer):
        self.peer = peer
        self._rx = b""

    def version(self):
        return struct.unpack("<I", self.peer.read(FT_VERSION))[0]

    def _handle(self):
        return self.peer.characteristic(FT_TRANSFER).handle

    def _send(self, data):
        self.peer.subscribe(FT_TRANSFER)
        step = self.peer.mtu - 3
        for k in range(0, len(data), step):
            self.peer.write(FT_TRANSFER, data[k : k + step], response=False)

    def _recv(self, n, timeout=10):
        """The next n bytes the board notified on the transfer characteristic."""
        handle, end = self._handle(), time.time() + timeout
        while len(self._rx) < n:
            got = self.peer._notified.pop(handle, [])
            self._rx += b"".join(got)
            if len(self._rx) >= n:
                break
            if time.time() > end:
                raise TimeoutError("%d of %d bytes from the file transfer" % (len(self._rx), n))
            self.peer._take(min(max(end - time.time(), 0), 1))
        data, self._rx = self._rx[:n], self._rx[n:]
        return data

    def _status(self, command, status):
        if status != 0x01:
            raise FileTransferError(command, status)

    def listdir(self, path):
        """The entries of the directory at path."""
        p = path.encode()
        self._send(struct.pack("<BBH", 0x50, 0, len(p)) + p)
        entries = []
        while True:
            command, status, plen, number, count, flags, mtime, size = struct.unpack(
                "<BBHIIIQI", self._recv(28)
            )
            self._status(command, status)
            if number >= count:
                return entries
            name = self._recv(plen).decode(errors="replace")
            entries.append(FileEntry(name, bool(flags & 1), size, mtime))

    def read(self, path, chunk=CHUNK):
        """The whole file at path, chunk bytes at a time, as READ then READ_PACING."""
        p = path.encode()
        self._send(struct.pack("<BBHII", 0x10, 0, len(p), 0, chunk) + p)
        data = b""
        while True:
            command, status, _, offset, total, size = struct.unpack("<BBHIII", self._recv(16))
            self._status(command, status)
            data += self._recv(size)
            if offset + size >= total:
                return data
            self._send(struct.pack("<BBHII", 0x12, 0x01, 0, offset + size, chunk))

    def write(self, path, data, mtime=None):
        """data to the file at path (created, or truncated to it); mtime in ns since 1970 (now by
        default). The mtime the board kept, FAT's 2 s apart, is returned."""
        p, data = path.encode(), data.encode() if isinstance(data, str) else bytes(data)
        mtime = time.time_ns() if mtime is None else mtime
        self._send(struct.pack("<BBHIQI", 0x20, 0, len(p), 0, mtime, len(data)) + p)
        while True:
            command, status, _, offset, kept, free = struct.unpack("<BBHIQI", self._recv(20))
            self._status(command, status)
            if free == 0:
                return kept
            piece = data[offset : offset + free]
            self._send(struct.pack("<BBHII", 0x22, 0x01, 0, offset, len(piece)) + piece)

    def delete(self, path):
        """The file, or the directory and all in it, at path."""
        p = path.encode()
        self._send(struct.pack("<BBH", 0x30, 0, len(p)) + p)
        command, status = struct.unpack("<BB", self._recv(2))
        self._status(command, status)

    def mkdir(self, path, mtime=None):
        p = path.encode()
        mtime = time.time_ns() if mtime is None else mtime
        self._send(struct.pack("<BBHIQ", 0x40, 0, len(p), 0, mtime) + p)
        command, status, _, _, _ = struct.unpack("<BBHIQ", self._recv(16))
        self._status(command, status)

    def move(self, old, new):
        o, n = old.encode(), new.encode()
        self._send(struct.pack("<BBHH", 0x60, 0, len(o), len(n)) + o + b"\0" + n)
        command, status = struct.unpack("<BB", self._recv(2))
        self._status(command, status)


class BleConsole:
    """The BLE workflow's serial service: the REPL and code.py's output, as the phone has them."""

    def __init__(self, peer):
        self.peer = peer

    def write(self, text):
        """Text (or bytes) typed, in pieces an MTU takes; notifications on first."""
        self.peer.subscribe(SERIAL_TX)
        data = text.encode() if isinstance(text, str) else bytes(text)
        for k in range(0, len(data), self.peer.mtu - 3):
            self.peer.write(SERIAL_RX, data[k : k + self.peer.mtu - 3], response=False)

    def read(self, timeout=1):
        """What the board wrote since the last call, waiting up to timeout for the first of it."""
        return b"".join(self.peer.notifications(SERIAL_TX, timeout)).decode(errors="replace")

    def interrupt(self):
        """Ctrl-C."""
        self.write(b"\x03")


class Peer:
    """The phone connected to the program: a GATT client over ATT PDUs through the control API."""

    MTU = 247  # asked for; the program's stack answers what it takes
    ADDRESS = "7c:10:c9:1c:37:89"  # the virtual controller's phone, with no identity behind it

    def __init__(self, board, address=None, identity=None, bond=None, auto_pair=True):
        """address: the one connected from (6 bytes, least significant first), random. identity:
        the phone's Identity, given away when it pairs. bond: a Bond it encrypts with at once, as
        a phone does with a board it knows. auto_pair: answer 0x05 Insufficient Authentication by
        pairing and 0x0F Insufficient Encryption by encrypting with the bond, then ask again."""
        self.board = board
        self.address = bytes(address) if address else address_bytes(self.ADDRESS)
        self.identity, self.bond, self.auto_pair = identity, bond, auto_pair
        self.mtu = 23
        self._notified = {}  # value handle: [values]
        self._services = None
        self._security_requests = []
        self._smp_pending = []  # PDUs taken from the program, not yet expected
        self._encrypted = False  # this link, by the phone's own encryption
        self._securing = False
        if bond is not None:
            self.encrypt(bond)
        answer = self._request(bytes([0x02]) + struct.pack("<H", self.MTU), 0x03)
        self.mtu = max(23, min(self.MTU, struct.unpack("<H", answer[1:3])[0]))

    def _send(self, pdu):
        self.board.request("POST", "/api/ble/att", pdu.hex())

    def _take(self, timeout):
        """The PDUs the program sent that are not notifications, waiting up to timeout for one.
        Notifications are kept for notifications(); an indication is confirmed. A request of the
        program's own is answered as a phone with no services it asks for would: an MTU Exchange
        with 23, anything else with Attribute Not Found."""
        pdus = self.board.request("GET", "/api/ble/att?wait=%d" % int(timeout * 1000))["pdus"]
        answers = []
        for hexed in pdus:
            pdu = bytes.fromhex(hexed)
            if pdu[:1] in (b"\x1b", b"\x1d"):
                handle = struct.unpack("<H", pdu[1:3])[0]
                self._notified.setdefault(handle, []).append(pdu[3:])
                if pdu[0] == 0x1D:
                    self._send(b"\x1e")
            elif pdu[:1] == b"\x02":
                self._send(b"\x03" + struct.pack("<H", 23))
            elif pdu and not pdu[0] & 0x40 and pdu[0] not in ATT_ANSWERS:
                handle = struct.unpack("<H", pdu[1:3])[0] if len(pdu) >= 3 else 0
                self._send(bytes([0x01, pdu[0]]) + struct.pack("<HB", handle, 0x0A))
            else:
                answers.append(pdu)
        return answers

    def serve(self, seconds):
        """Answers the program's requests for seconds (a phone's stack answers them on its own)."""
        end = time.time() + seconds
        while time.time() < end:
            self._take(min(max(end - time.time(), 0), 0.5))

    def _request(self, pdu, answer, timeout=5):
        """pdu sent; the program's answer of opcode answer, or AttError. ATT has one request at a
        time, so an answer to another one (late, after a timeout) is dropped. With auto_pair, a
        refusal for security is answered as a phone does (pairing, or its bond's encryption), and
        the request asked again once."""
        try:
            return self._request_once(pdu, answer, timeout)
        except AttError as e:
            # On an encrypted link, Just Works can do no better: the refusal stands.
            if (
                not self.auto_pair
                or self._securing
                or self._encrypted
                or e.code not in (0x05, 0x0F)
            ):
                raise
            self._securing = True
            try:
                if e.code == 0x0F and self.bond is not None:
                    self.encrypt(self.bond)
                else:
                    self.pair()
            finally:
                self._securing = False
            return self._request_once(pdu, answer, timeout)

    def _request_once(self, pdu, answer, timeout):
        self._send(pdu)
        end = time.time() + timeout
        while True:
            for got in self._take(min(max(end - time.time(), 0), 1)):
                if got[0] == 0x01 and got[1] == pdu[0]:
                    raise AttError(got[1], struct.unpack("<H", got[2:4])[0], got[4])
                if got[0] == answer:
                    return got
            if time.time() > end:
                raise TimeoutError("no answer to ATT 0x%02x" % pdu[0])

    def _each(self, opcode, answer, start, end, extra):
        """The entries of a request repeated over a handle range."""
        while start <= end:
            try:
                got = self._request(
                    bytes([opcode]) + struct.pack("<HH", start, end) + extra, answer
                )
            except AttError as e:
                if e.code == 0x0A:  # attribute not found: the range is done
                    return
                raise
            size = got[1]
            last = start
            for k in range(2, len(got) - size + 1, size):
                entry = got[k : k + size]
                last = struct.unpack("<H", entry[2:4] if opcode == 0x10 else entry[:2])[0]
                yield entry
            if last >= 0xFFFF:
                return
            start = last + 1

    def services(self):
        """The program's services, each with its characteristics."""
        if self._services is not None:
            return self._services
        found = []
        for e in self._each(0x10, 0x11, 1, 0xFFFF, struct.pack("<H", 0x2800)):
            s, end = struct.unpack("<HH", e[:4])
            found.append(Service(_uuid_from_le(e[4:]), s, end))
        for s in found:
            for e in self._each(0x08, 0x09, s.start, s.end, struct.pack("<H", 0x2803)):
                props, value = e[2], struct.unpack("<H", e[3:5])[0]
                s.characteristics.append(Characteristic(_uuid_from_le(e[5:]), value, props))
            # A characteristic's CCCD lies between its value and the next declaration.
            for i, c in enumerate(s.characteristics):
                last = (
                    s.characteristics[i + 1].handle - 2
                    if i + 1 < len(s.characteristics)
                    else s.end
                )
                if c.handle + 1 > last:
                    continue
                try:
                    got = self._request(b"\x04" + struct.pack("<HH", c.handle + 1, last), 0x05)
                except AttError:
                    continue
                size = 4 if got[1] == 1 else 18
                for k in range(2, len(got) - size + 1, size):
                    if got[k + 2 : k + size] == b"\x02\x29":
                        c.cccd = struct.unpack("<H", got[k : k + 2])[0]
        self._services = found
        return found

    def characteristic(self, u):
        u = uuid(u)
        for s in self.services():
            for c in s.characteristics:
                if c.uuid == u:
                    return c
        raise KeyError("no characteristic %s" % u)

    def read(self, u):
        """A characteristic's value, read whole (Read Blob for the rest of a long one)."""
        c = self.characteristic(u)
        value = self._request(b"\x0a" + struct.pack("<H", c.handle), 0x0B)[1:]
        while len(value) and len(value) % (self.mtu - 1) == 0:
            try:
                more = self._request(b"\x0c" + struct.pack("<HH", c.handle, len(value)), 0x0D)[1:]
            except AttError as e:
                if e.code == 0x07:  # invalid offset: the value ended exactly at the last answer
                    break
                raise
            if not more:
                break
            value += more
        return value

    def write(self, u, data, response=True):
        """A Write Request (answered) or a Write Command (not) of data."""
        c = self.characteristic(u)
        data = data.encode() if isinstance(data, str) else bytes(data)
        if len(data) > self.mtu - 3:
            raise ValueError("%d bytes is more than an MTU of %d takes" % (len(data), self.mtu))
        if response:
            self._request(b"\x12" + struct.pack("<H", c.handle) + data, 0x13)
        else:
            self._send(b"\x52" + struct.pack("<H", c.handle) + data)

    def subscribe(self, u):
        """A characteristic's notifications on; once is enough."""
        c = self.characteristic(u)
        if c.cccd is None:
            raise ValueError("%s has no notifications" % c.uuid)
        if not c.subscribed:
            self._request(b"\x12" + struct.pack("<HH", c.cccd, 1), 0x13)
            c.subscribed = True

    def notifications(self, u, timeout=1):
        """The values notified since the last call (notifications turned on at the first),
        waiting up to timeout for the first."""
        self.subscribe(u)
        c = self.characteristic(u)
        end = time.time() + timeout
        while not self._notified.get(c.handle) and time.time() < end:
            self._take(min(end - time.time(), 1))
        self._take(0)
        return self._notified.pop(c.handle, [])

    def uart_write(self, text):
        """Text (or bytes) to the program's Nordic UART service, in pieces an MTU takes; its TX
        notifications on first, as a phone's app has them."""
        self.subscribe(NUS_TX)
        data = text.encode() if isinstance(text, str) else bytes(text)
        for k in range(0, len(data), self.mtu - 3):
            self.write(NUS_RX, data[k : k + self.mtu - 3], response=False)

    def uart_read(self, timeout=1):
        """What the program wrote to its Nordic UART since the last call."""
        return b"".join(self.notifications(NUS_TX, timeout)).decode(errors="replace")

    @property
    def files(self):
        """The BLE workflow's file transfer service (a FileTransfer)."""
        if not hasattr(self, "_files"):
            self._files = FileTransfer(self)
        return self._files

    @property
    def console(self):
        """The BLE workflow's serial service (a BleConsole)."""
        if not hasattr(self, "_console"):
            self._console = BleConsole(self)
        return self._console

    def disconnect(self):
        self.board.request("POST", "/api/ble/disconnect")

    # SMP, channel 6: the phone pairs as the initiator (LE Legacy, Just Works), as a phone does.

    def _smp_send(self, pdu):
        self.board.request("POST", "/api/ble/smp", bytes(pdu).hex())

    def _smp_take(self, timeout):
        """The SMP PDUs the program sent, waiting up to timeout for one; Security Requests are kept
        for security_requests()."""
        pdus = self.board.request("GET", "/api/ble/smp?wait=%d" % int(timeout * 1000))["pdus"]
        out = []
        for hexed in pdus:
            pdu = bytes.fromhex(hexed)
            if pdu[:1] == b"\x0b":
                self._security_requests.append(pdu[1] if len(pdu) > 1 else 0)
            else:
                out.append(pdu)
        return out

    def _smp_expect(self, code, timeout=5):
        """The program's next SMP PDU, which should be of code: PairingFailed when it gave up."""
        end = time.time() + timeout
        while not self._smp_pending:
            self._smp_pending += self._smp_take(min(max(end - time.time(), 0), 1))
            if not self._smp_pending and time.time() > end:
                raise TimeoutError("no SMP 0x%02x from the program" % code)
        pdu = self._smp_pending.pop(0)
        if pdu[0] == 0x05:
            self._smp_pending = []
            raise PairingFailed(pdu[1] if len(pdu) > 1 else 0)
        if pdu[0] != code:
            raise RuntimeError("SMP 0x%02x where 0x%02x was due" % (pdu[0], code))
        return pdu

    def security_requests(self, timeout=0):
        """The Security Requests' AuthReq bytes the program sent since the last call (1: it asks
        for a bond), waiting up to timeout for one."""
        end = time.time() + timeout
        while True:
            self._smp_take(0)
            if self._security_requests or time.time() >= end:
                break
            time.sleep(0.02)
        got, self._security_requests = self._security_requests, []
        return got

    def _program_address(self):
        """(type, address bytes) the program advertised from: c1 takes the responder's."""
        s = self.board.request("GET", "/api/ble")
        return (0 if s["public"] else 1), address_bytes(s["address"])

    def pair(self, bond=True, max_key_size=16, tamper=None):
        """Pairing as the phone: a Bond (the keys and identities given) when the program bonded,
        else one with no keys. max_key_size below 7 is refused by the program (PairingFailed
        0x06); tamper="confirm" sends a wrong confirm value (0x04)."""
        rat, ra = self._program_address()
        give = 0x02 if (bond and self.identity is not None) else 0x00
        preq = bytes(
            [0x01, 0x03, 0x00, 0x01 if bond else 0x00, max_key_size, give, 0x03 if bond else 0x00]
        )
        self._smp_send(preq)
        prsp = self._smp_expect(0x02)
        key_size = min(max_key_size, prsp[4])
        mrand = os.urandom(16)
        confirm = smp_c1(bytes(16), mrand, preq, prsp, 1, self.address, rat, ra)
        if tamper == "confirm":
            confirm = bytes([confirm[0] ^ 0xFF]) + confirm[1:]
        self._smp_send(b"\x03" + confirm)
        sconfirm = self._smp_expect(0x03)[1:]
        self._smp_send(b"\x04" + mrand)
        srand = self._smp_expect(0x04)[1:]
        if smp_c1(bytes(16), srand, preq, prsp, 1, self.address, rat, ra) != sconfirm:
            self._smp_send(b"\x05\x04")
            raise PairingFailed(0x04, sent=True)
        stk = smp_s1(bytes(16), srand, mrand)[:key_size] + bytes(16 - key_size)
        self._encrypt(stk, 0, bytes(8))
        b = Bond(key_size)
        if prsp[6] & 0x01:
            b.ltk = self._smp_expect(0x06)[1:17]
            central_id = self._smp_expect(0x07)
            b.ediv, b.rand = struct.unpack("<H", central_id[1:3])[0], central_id[3:11]
        if prsp[6] & 0x02:
            b.irk = self._smp_expect(0x08)[1:17]
            address = self._smp_expect(0x09)
            b.identity_type, b.identity = address[1], address_text(address[2:8])
        if prsp[5] & 0x02:
            self._smp_send(b"\x08" + self.identity.irk)
            self._smp_send(b"\x09\x01" + self.identity.address)
            b.phone = self.identity
        # A barrier, harmless to ask: when ATT answers, the program has taken the keys.
        self._request_once(b"\x10" + struct.pack("<HHH", 1, 1, 0x2800), 0x11, 5)
        if b.ltk is not None:
            self.bond = b
        return b

    def _encrypt(self, ltk, ediv, rand):
        body = {"ltk": bytes(ltk).hex(), "ediv": ediv, "rand": bytes(rand).hex()}
        status = self.board.request("POST", "/api/ble/encrypt", body)["status"]
        if status != 0:
            raise EncryptError(status)
        self._encrypted = True

    def encrypt(self, bond, ltk=None):
        """The link encrypted with a bond's key, as a phone does on reconnecting (ltk: another key,
        for a test): EncryptError 0x06 when the program has no bond of it, 0x3D for a wrong key."""
        self._encrypt(bond.ltk if ltk is None else ltk, bond.ediv, bond.rand)
        self.bond = bond

    @property
    def encrypted(self):
        """Whether the phone's link is encrypted."""
        links = self.board.request("GET", "/api/ble").get("links", [])
        return any(link["role"] == "peripheral" and link["encrypted"] for link in links)


class Ble:
    """The phone's side of the virtual Bluetooth controller."""

    def __init__(self, board):
        self.board = board

    def state(self):
        return self.board.request("GET", "/api/ble")

    def scan(self):
        """What the program advertises now, or None when it advertises nothing."""
        s = self.state()
        return Advertisement(s) if s.get("advertising") else None

    def discover(self, timeout=15):
        """Discovery mode, as reset plus the boot button on a board: the lab resets, bonds erased,
        and advertises publicly as CIRCUITPY. Returns when the new boot answers."""
        before = self.board.info()["boot"]
        self.board.request("POST", "/api/ble/discover")
        end = time.time() + timeout
        while time.time() < end:
            try:
                if self.board.info()["boot"] != before:
                    return
            except (OSError, http.client.HTTPException, ApiError):
                pass
            time.sleep(0.05)
        raise TimeoutError("no new boot after discover")

    def connect(self, timeout=5, identity=None, bond=None, auto_pair=True, pair=False):
        """The phone joins the program's advertisement: a Peer, its MTU exchanged. Waits up to
        timeout for the program to advertise; any other refusal is raised at once. identity: the
        phone's (an Identity): it connects from a new resolvable private address of it. bond: it
        comes back as the phone of that Bond, from a new address, and encrypts at once. pair: it
        pairs (bonding) right after connecting."""
        if bond is not None and bond.phone is not None:
            identity = bond.phone
        address = identity.rpa() if identity is not None else None
        body = {"address": address_text(address), "type": "random"} if address else None
        end = time.time() + timeout
        while True:
            try:
                self.board.request("POST", "/api/ble/connect", body)
                break
            except ApiError as e:
                if "advertises nothing" not in e.message or time.time() > end:
                    raise
                time.sleep(0.05)
        p = Peer(self.board, address=address, identity=identity, bond=bond, auto_pair=auto_pair)
        if pair and bond is None:
            p.pair()
        return p

    def bonds(self, keys=False):
        """The program's bond store, as /api/ble/bonds gives it: a list of bonds."""
        return self.board.request("GET", "/api/ble/bonds" + ("?keys=1" if keys else ""))["bonds"]

    def erase_bonds(self):
        """The program's erase_bonding(): the bonds of this controller's identity, their number."""
        return self.board.request("POST", "/api/ble/bonds/erase")["erased"]


# ---- Bluetooth LE security: AES-128 and SMP's c1, s1 and ah (Core Vol 3 Part H 2.2) ----
#
# Test-grade and slow, standard library only: the phone's side of LE Legacy pairing. SMP's values
# go least significant byte first, as on the air; AES takes and gives the most significant first.


def _aes_sbox():
    sbox, p, q = [0] * 256, 1, 1
    while True:
        # p runs through the multiplicative group of GF(2^8), q through its inverses.
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        r = lambda x, s: ((x << s) | (x >> (8 - s))) & 0xFF
        sbox[p] = q ^ r(q, 1) ^ r(q, 2) ^ r(q, 3) ^ r(q, 4) ^ 0x63
        if p == 1:
            break
    sbox[0] = 0x63
    return sbox


_SBOX = _aes_sbox()


def _xtime(b):
    return ((b << 1) ^ (0x1B if b & 0x80 else 0)) & 0xFF


def aes128(key, block):
    """AES-128 encryption of one 16-byte block (FIPS-197), most significant byte first."""
    w = [list(key[i : i + 4]) for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = [_SBOX[t[1]] ^ rcon, _SBOX[t[2]], _SBOX[t[3]], _SBOX[t[0]]]
            rcon = _xtime(rcon)
        w.append([a ^ b for a, b in zip(w[i - 4], t)])
    s = [b ^ k for b, k in zip(block, sum(w[0:4], []))]
    for rnd in range(1, 11):
        s = [_SBOX[b] for b in s]
        s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]  # rows shifted left by their number
        if rnd < 10:
            m = []
            for c in range(4):
                a = s[4 * c : 4 * c + 4]
                x = a[0] ^ a[1] ^ a[2] ^ a[3]
                m += [a[i] ^ x ^ _xtime(a[i] ^ a[(i + 1) % 4]) for i in range(4)]
            s = m
        s = [b ^ k for b, k in zip(s, sum(w[4 * rnd : 4 * rnd + 4], []))]
    return bytes(s)


def smp_e(key, data):
    """SMP's e(): AES-128 of values given and returned least significant byte first."""
    return aes128(bytes(reversed(key)), bytes(reversed(data)))[::-1]


def _xor(a, b):
    return bytes(x ^ y for x, y in zip(a, b))


def smp_c1(k, r, preq, prsp, iat, ia, rat, ra):
    """The confirm value: k, r 16 bytes; preq, prsp the 7-byte Pairing Request and Response; the
    initiator's and responder's address types (0 public, 1 random) and addresses (6 bytes), all
    least significant byte first."""
    p1 = bytes([iat, rat]) + bytes(preq) + bytes(prsp)
    p2 = bytes(ra) + bytes(ia) + bytes(4)
    return smp_e(k, _xor(smp_e(k, _xor(r, p1)), p2))


def smp_s1(k, r1, r2):
    """The STK: e(k, r1's and r2's low halves), least significant byte first."""
    return smp_e(k, bytes(r2[:8]) + bytes(r1[:8]))


def smp_ah(k, r):
    """The random address hash of a 3-byte prand under an IRK k."""
    return smp_e(k, bytes(r[:3]) + bytes(13))[:3]


def _le(text):
    """A value written most significant first ("5783 d521...") as its bytes least significant first."""
    return bytes.fromhex(text.replace(" ", ""))[::-1]


def address_text(le):
    """A device address's 6 bytes (least significant first) as aa:bb:cc:dd:ee:ff."""
    return ":".join("%02x" % b for b in reversed(bytes(le)))


def address_bytes(text):
    """aa:bb:cc:dd:ee:ff as its 6 bytes, least significant first."""
    return bytes.fromhex(text.replace(":", ""))[::-1]


# Pairing Failed's reasons (Core Vol 3 Part H 3.5.5).
SMP_REASONS = {
    0x01: "passkey entry failed",
    0x02: "OOB not available",
    0x03: "authentication requirements",
    0x04: "confirm value failed",
    0x05: "pairing not supported",
    0x06: "encryption key size",
    0x07: "command not supported",
    0x08: "unspecified reason",
    0x09: "repeated attempts",
    0x0A: "invalid parameters",
}


class PairingFailed(Exception):
    """Pairing Failed, from the program or sent to it: its reason."""

    def __init__(self, reason, sent=False):
        super().__init__(
            "pairing failed 0x%02x (%s)%s"
            % (reason, SMP_REASONS.get(reason, "unknown"), ", sent by the phone" if sent else "")
        )
        self.reason = reason


class EncryptError(Exception):
    """The phone's encryption did not start: 0x06 (PIN or Key Missing: the program has no such
    key) or 0x3D (MIC failure: the keys differ, the link is gone)."""

    def __init__(self, status):
        super().__init__("encryption failed: 0x%02x" % status)
        self.status = status


class Identity:
    """A phone's identity: its IRK and identity address (random static). rpa() is a new resolvable
    private address of it, as a phone connects from."""

    def __init__(self, irk=None, address=None):
        self.irk = irk or os.urandom(16)
        a = bytearray(address_bytes(address) if address else os.urandom(6))
        a[5] |= 0xC0
        self.address = bytes(a)

    def rpa(self):
        prand = bytearray(os.urandom(3))
        prand[2] = (prand[2] & 0x3F) | 0x40
        return smp_ah(self.irk, prand) + bytes(prand)

    def __repr__(self):
        return "Identity(%s)" % address_text(self.address)


class Bond:
    """What a pairing left the phone: the program's LTK, EDIV, Rand and key size, its IRK and
    identity address (type and text); the phone's Identity, if it gave one."""

    def __init__(self, key_size):
        self.key_size = key_size
        self.ltk, self.ediv, self.rand = None, 0, bytes(8)
        self.irk, self.identity, self.identity_type = None, None, None
        self.phone = None

    def __repr__(self):
        return "Bond(%s, ediv=0x%04x, key=%d)" % (self.identity, self.ediv, self.key_size)


# ---- Bluetooth LE: the air around the program, the devices it scans for and connects to ----


def _uuid_le(u):
    """A UUID as ATT carries it, little-endian: 2 bytes for one on the Bluetooth base, else 16."""
    full = uuid(u)
    if full.endswith(BASE_UUID) and full.startswith("0000"):
        return struct.pack("<H", int(full[4:8], 16))
    return bytes(reversed(bytes.fromhex(full.replace("-", ""))))


class GattCharacteristic:
    """A characteristic of a Peripheral's GATT server: its UUID, its properties by name (as
    Characteristic.names() gives them), its value. error: the ATT error code every read and write
    of it gets (0x02 read not permitted, 0x03 write not permitted, 0x05 insufficient
    authentication ...), or "none": reads of it get no answer at all."""

    def __init__(self, u, properties=("read",), value=b"", error=None):
        self.uuid = uuid(u)
        self.properties = sum(bit for bit, name in PROPERTIES if name in properties)
        self.value = bytes(value)
        self.error = error
        self.decl = self.handle = self.cccd = None  # its handles, which the Peripheral gives
        self.cccd_value = 0  # what the program wrote to its CCCD
        self.writes = []  # the values the program wrote, oldest first

    def __repr__(self):
        return "GattCharacteristic(%s, 0x%04x)" % (self.uuid, self.handle or 0)


class GattService:
    def __init__(self, u, characteristics=()):
        self.uuid = uuid(u)
        self.characteristics = list(characteristics)
        self.start = self.end = None


_next_address = [0]


class Peripheral:
    """A device in the air around the program: it advertises and, when the program connects, it
    serves GATT as a sensor does: MTU Exchange, discovery (Read By Group Type, Find By Type Value,
    Read By Type, Find Information), Read, Read Blob, Write Request and Command. It notifies and
    indicates once the program has turned that on. The mirror of Peer, which is a client. It
    lives in an Air (air.add(device)), whose threads feed it the program's frames.

    data, scan_response: raw advertising bytes; by default the flags, the services' UUIDs and the
    name (in the scan response when it does not fit)."""

    MTU = 247  # its own receive MTU, answered to an exchange; the link uses the smaller one

    def __init__(
        self,
        name=None,
        services=(),
        address=None,
        random=True,
        data=None,
        scan_response=None,
        connectable=True,
        rssi=-60,
    ):
        if address is None:
            _next_address[0] += 1
            address = "c0:00:00:00:%02x:%02x" % divmod(_next_address[0], 256)
        self.name, self.address, self.random = name, address.lower(), random
        self.connectable, self.rssi = connectable, rssi
        self.services = [
            GattService(
                0x1800,
                [
                    GattCharacteristic(0x2A00, ("read",), (name or "").encode()),
                    GattCharacteristic(0x2A01, ("read",), b"\x00\x00"),
                ],
            ),
            GattService(0x1801, [GattCharacteristic(0x2A05, ("indicate",), b"")]),
        ] + list(services)
        h = 1
        for s in self.services:
            s.start, h = h, h + 1
            for c in s.characteristics:
                c.decl, c.handle, h = h, h + 1, h + 2
                if c.properties & 0x30:  # notify or indicate: a CCCD after the value
                    c.cccd, h = h, h + 1
            s.end = h - 1
        self.data, self.scan_response = self._advertisement(services, data, scan_response)
        self.air = None
        self.board = None  # the index in its Air of the board connected to it, or None
        self.advertising = True
        self.mtu = 23
        self.confirmations = 0  # indications the program confirmed

    def _advertisement(self, services, data, scan_response):
        if data is not None:
            return bytes(data), bytes(scan_response or b"")
        ad = bytes([2, 1, 6])
        for size, kind in ((2, 0x03), (16, 0x07)):
            ids = [_uuid_le(s.uuid) for s in services if len(_uuid_le(s.uuid)) == size]
            if ids:
                ad += bytes([1 + size * len(ids), kind]) + b"".join(ids)
        sr = bytes(scan_response or b"")
        if self.name:
            name = self.name.encode()
            field = bytes([1 + len(name), 0x09]) + name
            if len(ad) + len(field) <= 31:
                ad += field
            elif not sr:
                sr = field[:31]
        return ad, sr

    def __repr__(self):
        return "Peripheral(%r, %s)" % (self.name, self.address)

    # ---- what the program sees ----

    def _peer(self):
        """Its entry in a controller's air: POST /api/ble/peer."""
        if not self.advertising:
            return {"address": self.address, "advertising": False}
        return {
            "address": self.address,
            "random": self.random,
            "data": self.data.hex(),
            "scan_response": self.scan_response.hex(),
            "connectable": self.connectable,
            "rssi": self.rssi,
        }

    def _characteristics(self):
        return [c for s in self.services for c in s.characteristics]

    def characteristic(self, u):
        u = uuid(u)
        for c in self._characteristics():
            if c.uuid == u:
                return c
        raise KeyError("no characteristic %s" % u)

    def _attributes(self):
        """(handle, type, its value) of every attribute, in handle order."""
        out = []
        for s in self.services:
            out.append((s.start, uuid(0x2800), _uuid_le(s.uuid), None))
            for c in s.characteristics:
                decl = bytes([c.properties]) + struct.pack("<H", c.handle) + _uuid_le(c.uuid)
                out.append((c.decl, uuid(0x2803), decl, None))
                out.append((c.handle, c.uuid, c.value, c))
                if c.cccd:
                    out.append((c.cccd, uuid(0x2902), struct.pack("<H", c.cccd_value), None))
        return out

    def _att(self, pdu):
        """The answer to an ATT PDU from the program, or None."""
        op = pdu[0]

        def error(code, handle=0):
            return bytes([0x01, op]) + struct.pack("<HB", handle, code)

        def packed(answer, entries):
            """A response of entries of the first one's length, as many as the MTU takes."""
            if not entries:
                return None
            size = len(entries[0])
            out = bytes([answer, size])
            for e in entries:
                if len(e) != size or len(out) + size > self.mtu:
                    break
                out += e
            return out

        if op == 0x02 and len(pdu) == 3:  # MTU Exchange
            self.mtu = max(23, min(self.MTU, struct.unpack("<H", pdu[1:3])[0]))
            return b"\x03" + struct.pack("<H", self.MTU)
        if op in (0x04, 0x06, 0x08, 0x10) and len(pdu) >= 5:
            start, end = struct.unpack("<HH", pdu[1:5])
            if start == 0 or start > end:
                return error(0x01, start)
            if op == 0x10:  # Read By Group Type: the primary services
                if _uuid_from_le(pdu[5:]) != uuid(0x2800):
                    return error(0x10, start)
                found = [
                    struct.pack("<HH", s.start, s.end) + _uuid_le(s.uuid)
                    for s in self.services
                    if start <= s.start <= end
                ]
                return packed(0x11, found) or error(0x0A, start)
            if op == 0x06:  # Find By Type Value: the services of a UUID
                kind, value = struct.unpack("<H", pdu[5:7])[0], pdu[7:]
                found = [
                    struct.pack("<HH", s.start, s.end)
                    for s in self.services
                    if kind == 0x2800 and start <= s.start <= end and _uuid_le(s.uuid) == value
                ]
                found = found[: (self.mtu - 1) // 4]
                return b"\x07" + b"".join(found) if found else error(0x0A, start)
            attributes = [a for a in self._attributes() if start <= a[0] <= end]
            if op == 0x04:  # Find Information: handles and types
                found = [struct.pack("<H", h) + _uuid_le(t) for h, t, _, _ in attributes]
                if not found:
                    return error(0x0A, start)
                rsp = packed(0x05, found)
                return bytes([0x05, 1 if len(found[0]) == 4 else 2]) + rsp[2:]
            # Read By Type: characteristic declarations, or the first value of a type
            kind = _uuid_from_le(pdu[5:])
            if kind == uuid(0x2803):
                found = [struct.pack("<H", h) + v for h, t, v, _ in attributes if t == kind]
                return packed(0x09, found) or error(0x0A, start)
            for h, t, v, c in attributes:
                if t == kind:
                    code = self._refused(c, read=True)
                    if code:
                        return error(code, h)
                    v = v[: self.mtu - 4]
                    return bytes([0x09, 2 + len(v)]) + struct.pack("<H", h) + v
            return error(0x0A, start)
        if op in (0x0A, 0x0C) and len(pdu) >= 3:  # Read, Read Blob
            handle = struct.unpack("<H", pdu[1:3])[0]
            offset = struct.unpack("<H", pdu[3:5])[0] if op == 0x0C and len(pdu) >= 5 else 0
            for h, _, v, c in self._attributes():
                if h == handle:
                    if c is not None and c.error == "none":
                        return None  # a device that never answers
                    code = self._refused(c, read=True)
                    if code:
                        return error(code, h)
                    if offset > len(v):
                        return error(0x07, h)
                    return bytes([op + 1]) + v[offset : offset + self.mtu - 1]
            return error(0x01, handle)
        if op in (0x12, 0x52) and len(pdu) >= 3:  # Write Request, Write Command
            handle, value = struct.unpack("<H", pdu[1:3])[0], pdu[3:]
            for c in self._characteristics():
                if handle == c.cccd:
                    c.cccd_value = struct.unpack("<H", (value + b"\0\0")[:2])[0]
                    break
                if handle == c.handle:
                    code = self._refused(c, read=False, command=op == 0x52)
                    if code:
                        return error(code, handle) if op == 0x12 else None
                    c.value = bytes(value)
                    c.writes.append(bytes(value))
                    break
            else:
                return error(0x03 if handle else 0x01, handle) if op == 0x12 else None
            if self.air is not None:
                self.air.lock.notify_all()
            return b"\x13" if op == 0x12 else None
        if op == 0x1E:  # Handle Value Confirmation
            self.confirmations += 1
            if self.air is not None:
                self.air.lock.notify_all()
            return None
        if op & 0x40 or op in ATT_ANSWERS:
            return None  # commands and answers get none
        return error(0x06)

    @staticmethod
    def _refused(c, read, command=False):
        """The ATT error a read or a write of a characteristic's value gets, or 0."""
        if c is None:
            return 0 if read else 0x03
        if c.error:
            return c.error
        if read:
            return 0 if c.properties & 0x02 else 0x02
        return 0 if c.properties & (0x04 if command else 0x08) else 0x03

    # ---- what it does ----

    def _send(self, pdu):
        """pdu to the connected program; False when there is none (or its link just ended)."""
        if self.board is None:
            return False
        return self.air._post(
            self.air.boards[self.board],
            {"address": self.address, "cid": 4, "data": pdu.hex()},
            "/api/ble/l2cap",
        )

    def _value_pdu(self, opcode, c, value):
        c.value = bytes(value)
        return bytes([opcode]) + struct.pack("<H", c.handle) + c.value[: self.mtu - 3]

    def notify(self, u, value):
        """A characteristic's new value, notified when the program turned notifications on; True
        when it went out."""
        with self.air.lock:
            c = self.characteristic(u)
            pdu = self._value_pdu(0x1B, c, value)
            return bool(c.cccd_value & 1) and self._send(pdu)

    def indicate(self, u, value):
        """As notify(), as an indication: confirmations counts the program's answers."""
        with self.air.lock:
            c = self.characteristic(u)
            pdu = self._value_pdu(0x1D, c, value)
            return bool(c.cccd_value & 2) and self._send(pdu)

    def written(self, u):
        """The values the program wrote to a characteristic since the last call."""
        with self.air.lock:
            c = self.characteristic(u)
            got, c.writes = c.writes, []
            return got

    def wait_written(self, u, timeout=5):
        """written(), waiting up to timeout for a first value."""
        end = time.time() + timeout
        with self.air.lock:
            c = self.characteristic(u)
            while not c.writes and time.time() < end:
                self.air.lock.wait(max(0.01, end - time.time()))
            return self.written(u)

    def wait_connected(self, connected=True, timeout=5):
        """Whether a program is connected to it (or not, connected=False) within timeout."""
        end = time.time() + timeout
        with self.air.lock:
            while (self.board is not None) != connected and time.time() < end:
                self.air.lock.wait(max(0.01, end - time.time()))
            return (self.board is not None) == connected

    @property
    def connected(self):
        return self.board is not None

    def disconnect(self):
        """It ends its link to the program (0x13, the remote user ended it)."""
        with self.air.lock:
            board = self.board
        if board is not None:
            self.air.boards[board].request(
                "POST", "/api/ble/disconnect", {"address": self.address}
            )

    # ---- what its Air tells it ----

    def _connected(self, board, event):
        self.board, self.mtu, self.advertising = board, 23, False
        self.air._post_device(self, skip=board)

    def _disconnected(self):
        self.board, self.mtu = None, 23
        for c in self._characteristics():
            c.cccd_value = 0
        self.advertising = True
        self.air._post_device(self)

    def _frame(self, cid, data):
        if cid == 4 and data:
            answer = self._att(data)
            if answer:
                self._send(answer)


# Responses, notifications, indications and the confirmation: what nobody answers.
ATT_ANSWERS = {0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F, 0x11, 0x13, 0x17, 0x19}
ATT_ANSWERS |= {0x1B, 0x1D, 0x1E, 0x21, 0x23}


class devices:
    """Ready Peripherals."""

    HEART_RATE = 0x180D
    MEASUREMENT = 0x2A37
    BODY_SENSOR_LOCATION = 0x2A38

    @staticmethod
    def heart_rate(bpm=72, name="HRM", **kw):
        """A heart rate sensor (0x180D): measurement notified (beat(bpm)), body sensor location
        read (1, the chest)."""
        d = Peripheral(
            name,
            [
                GattService(
                    devices.HEART_RATE,
                    [
                        GattCharacteristic(devices.MEASUREMENT, ("notify",), bytes([0, bpm])),
                        GattCharacteristic(devices.BODY_SENSOR_LOCATION, ("read",), b"\x01"),
                    ],
                )
            ],
            **kw,
        )

        def beat(bpm):
            """A measurement of bpm notified; True when it went out."""
            return d.notify(devices.MEASUREMENT, bytes([0, bpm]))

        d.beat = beat
        return d

    @staticmethod
    def uart(name="UART", **kw):
        """A Nordic UART device: send(text) notifies on TX, received() is what the program wrote
        to RX."""
        d = Peripheral(
            name,
            [
                GattService(
                    NUS_SERVICE,
                    [
                        GattCharacteristic(NUS_RX, ("write", "write_no_response")),
                        GattCharacteristic(NUS_TX, ("notify",)),
                    ],
                )
            ],
            **kw,
        )

        def send(text):
            data = text.encode() if isinstance(text, str) else bytes(text)
            sent = True
            for k in range(0, len(data), d.mtu - 3):
                sent = d.notify(NUS_TX, data[k : k + d.mtu - 3]) and sent
            return sent

        def received(timeout=0):
            got = d.wait_written(NUS_RX, timeout) if timeout else d.written(NUS_RX)
            return b"".join(got).decode(errors="replace")

        d.send, d.received = send, received
        return d

    @staticmethod
    def beacon(data, rssi=-60, **kw):
        """A beacon: raw advertising data (an iBeacon or Eddystone frame), not connectable."""
        return Peripheral(None, (), data=data, connectable=False, rssi=rssi, **kw)


class Air:
    """The air around programs: the Peripherals in it, and between two programs, what one
    advertises reaches the others and a connection one makes to another runs both ways (CLUE to
    CLUE). A thread per board reads its events in order; the devices' answers and the relay run
    under one lock. Devices are posted again every repeat seconds while a program scans, as a
    real one advertises on. Use it with `with`."""

    def __init__(self, *boards, repeat=0.25):
        import threading

        self.boards = list(boards)
        self.devices = []
        self.repeat = repeat
        self.lock = threading.Condition(threading.RLock())
        self.events = [[] for _ in boards]  # every event of each board, for a test to read
        self._stop = threading.Event()
        self._scanning = [False] * len(boards)
        self._adv = [None] * len(boards)  # each board's advertising, while it advertises
        self._links = {}  # (board, peer address there) -> (other board, address there)
        self._threads = [
            threading.Thread(target=self._read, args=(i,), daemon=True) for i in range(len(boards))
        ]
        if repeat:
            self._threads.append(threading.Thread(target=self._repeat, daemon=True))
        for t in self._threads:
            t.start()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        self._stop.set()
        for t in self._threads:
            t.join(3)

    def add(self, device):
        """A device into the air: every program sees it advertise. Returns it."""
        with self.lock:
            device.air = self
            device.board, device.advertising = None, True
            self.devices.append(device)
            self._post_device(device)
        return device

    def remove(self, device):
        """The device leaves the air: gone from every program, its link ended (0x13)."""
        with self.lock:
            if device in self.devices:
                self.devices.remove(device)
            for b in self.boards:
                self._post(b, {"address": device.address, "gone": True})
            device.board = None

    def wait(self, test, timeout=5):
        """Waits up to timeout for test() (called under the lock) to be true; its last value."""
        end = time.time() + timeout
        with self.lock:
            while not test() and time.time() < end:
                self.lock.wait(min(0.1, max(0.01, end - time.time())))
            return test()

    # ---- under the lock ----

    @staticmethod
    def _post(board, body, path="/api/ble/peer"):
        try:
            board.request("POST", path, body)
            return True
        except (ApiError, OSError, http.client.HTTPException):
            return False

    def _post_device(self, device, skip=None):
        for i, b in enumerate(self.boards):
            if i != skip:
                self._post(b, device._peer())

    def _post_lab(self, i, j):
        """Board i's advertising as a peer in board j's air."""
        adv = self._adv[i]
        if adv is None:
            return
        if not adv["on"]:
            body = {"address": adv["address"], "advertising": False}
        else:
            body = {
                "address": adv["address"],
                "random": adv["random"],
                "data": adv["data"],
                "scan_response": adv["scan_response"],
                "connectable": adv["connectable"],
                "rssi": -50,
            }
        self._post(self.boards[j], body)

    def _device(self, address):
        for d in self.devices:
            if d.address == address:
                return d
        return None

    def _lab(self, address):
        for i, adv in enumerate(self._adv):
            if adv is not None and adv["address"] == address:
                return i
        return None

    def _event(self, i, e):
        kind = e["kind"]
        b = self.boards[i]
        if kind == "adv":
            self._adv[i] = e
            for j in range(len(self.boards)):
                if j != i:
                    self._post_lab(i, j)
        elif kind == "scan":
            self._scanning[i] = e["on"]
        elif kind == "connected":
            d = self._device(e["peer"])
            if d is not None:
                d._connected(i, e)
            elif e["role"] == "central":
                j = self._lab(e["peer"])
                if j is not None and j != i:
                    self._links[(i, e["peer"])] = (j, e["local"])
                    self._links[(j, e["local"])] = (i, e["peer"])
                    body = {"address": e["local"], "random": e["local_random"]}
                    if not self._post(self.boards[j], body, "/api/ble/connect"):
                        self._links.pop((i, e["peer"]), None)
                        self._links.pop((j, e["local"]), None)
                        self._post(b, {"address": e["peer"]}, "/api/ble/disconnect")
        elif kind == "disconnected":
            d = self._device(e["peer"])
            if d is not None and d.board == i:
                d._disconnected()
            link = self._links.pop((i, e["peer"]), None)
            if link is not None:
                j, address = link
                self._links.pop((j, address), None)
                self._post(self.boards[j], {"address": address}, "/api/ble/disconnect")
        elif kind == "l2cap":
            d = self._device(e["peer"])
            if d is not None and d.board == i:
                d._frame(e["cid"], bytes.fromhex(e["data"]))
            link = self._links.get((i, e["peer"]))
            if link is not None:
                j, address = link
                body = {"address": address, "cid": e["cid"], "data": e["data"]}
                self._post(self.boards[j], body, "/api/ble/l2cap")
        self.lock.notify_all()

    # ---- the threads ----

    def _read(self, i):
        b, since = self.boards[i], 0
        while not self._stop.is_set():
            try:
                r = b.request("GET", "/api/ble/events?since=%d&wait=300" % since, timeout=30)
            except (ApiError, OSError, http.client.HTTPException):
                if b.process is not None and b.process.poll() is not None:
                    return
                self._stop.wait(0.2)
                continue
            since = r["next"]
            for e in r["events"]:
                with self.lock:
                    self.events[i].append(e)
                    try:
                        self._event(i, e)
                    except (ApiError, OSError, http.client.HTTPException):
                        pass  # a link that ended meanwhile: the next events say so

    def _repeat(self):
        while not self._stop.wait(self.repeat):
            with self.lock:
                for j, scanning in enumerate(self._scanning):
                    if not scanning:
                        continue
                    for d in self.devices:
                        if d.advertising:
                            self._post(self.boards[j], d._peer())
                    for i, adv in enumerate(self._adv):
                        if i != j and adv is not None and adv["on"]:
                            self._post_lab(i, j)


class bluefruit:
    """Bluefruit Connect's packets, as its app sends them to the Nordic UART."""

    @staticmethod
    def _packet(body):
        return body + bytes([~sum(body) & 0xFF])

    @staticmethod
    def color(r, g, b):
        return bluefruit._packet(b"!C" + bytes([r, g, b]))

    @staticmethod
    def button(n, pressed):
        return bluefruit._packet(b"!B" + b"%d" % n + (b"1" if pressed else b"0"))

    @staticmethod
    def accelerometer(x, y, z):
        return bluefruit._packet(b"!A" + struct.pack("<fff", x, y, z))


def _until(b, texts, timeout):
    """(the console read on from where b was, the first of texts in it or None) within timeout.
    Read as it comes, so the text before a hard fault's reset is kept; a program that has ended
    ends it too."""
    out, end = "", time.time() + timeout
    while True:
        try:
            r = b.console(timeout=0.1)
        except (OSError, http.client.HTTPException):
            return out, None
        b.offset = r["next"]
        out += r["data"]
        seen = next((t for t in texts if t in out), None)
        if seen or time.time() > end or (b.process is not None and b.process.poll() is not None):
            return out, seen


def run(argv):
    """cpapi.py run PROGRAM CODE: CODE (a .py file as code.py, or a directory as the drive) run
    headless until code.py ends or --wait's text comes; its console printed, and the display if
    asked. Exit status 0; 1 when it did not end in time, or the program ended; 2 after a hard
    fault; 3 when code.py ended in an exception; 4 when it reported a fail (lab.result); 64 for
    wrong arguments."""
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
    ap.add_argument("--code", dest="code_file", help="a file on the drive run in place of code.py")
    ap.add_argument(
        "--wav", help="record the audio outputs; save the first as this WAV after the last run"
    )
    try:
        a = ap.parse_args(argv)
    except SystemExit as e:
        return 64 if e.code else 0
    # The console may have bytes the terminal's code page cannot print.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

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
        a.program,
        drive,
        board_file=a.board_file,
        board=a.board,
        fast_time=a.fast_time,
        code_file=a.code_file,
        audio_capture=bool(a.wav),
    ) as b:
        for i in range(a.runs):
            if i:
                b.reload()
            out, seen = _until(b, texts, a.timeout)
            print("--- run %d" % (i + 1))
            print(plain(out).rstrip())
            if seen == "You are in safe mode":
                status = 2
                break
            if b.process is not None and b.process.poll() is not None:
                print("--- the program ended, status %d" % b.process.returncode)
                status = 1
                break
            if seen is None:
                print("--- not ended in %g s" % a.timeout)
                status = 1
                break
            if "Traceback (most recent call last)" in out:
                status = 3
            if not a.wait:
                left = b.after_run()
                reported = left["result"]
                if reported:
                    print("--- result: " + ": ".join(x for x in reported.values() if x))
                    if reported["status"] == "fail" and status == 0:
                        status = 4
                if left["faults_fired"]:
                    print("--- faults fired: " + _faults(left["faults_fired"]))
                if left["leaked_pins"]:
                    print("--- still claimed after the run: " + ", ".join(left["leaked_pins"]))
                # The first run's change is against boot.py's VM, so it counts from the second.
                if i and left["outside_heap_change"]:
                    print(
                        "--- outside the heap: %+d bytes since the run before"
                        % left["outside_heap_change"]
                    )
        if a.wav and b.process.poll() is None:
            recordings = b.audio()
            for r in recordings:
                print("--- audio " + r.summary())
            if recordings:
                recordings[0].save(a.wav)
                print("--- audio saved:", a.wav)
            else:
                print("--- no audio played")
        if status in (0, 3) and (a.png or a.colors):
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


def _code_output(out):
    """What code.py printed in one run's console, without the supervisor's lines."""
    text = plain(out)
    if "code.py output:" in text:
        text = text.split("code.py output:", 1)[1]
    return text.split("Code done running.", 1)[0].strip()


def _summed(fired):
    """Faults fired over several runs, each point and action once with its times added up."""
    times = {}
    for f in fired:
        times[(f["point"], f["action"])] = times.get((f["point"], f["action"]), 0) + f["times"]
    return [{"point": p, "action": a, "times": t} for (p, a), t in times.items()]


def _faults(fired):
    """Faults fired, as a line: \"sd.write error x3, i2c nack x1\"."""
    return ", ".join("%s %s x%d" % (f["point"], f["action"], f["times"]) for f in fired)


# A run's outcome when it ended well and reported a result with lab.result.
REPORTED = {"pass": "passed", "fail": "failed", "skip": "skipped"}


def _trials(program, code, a):
    """Each run of code on program: {"outcome", "detail", "last", "leaked"}."""
    import shutil
    import tempfile

    drive = os.path.join(tempfile.mkdtemp(), "CIRCUITPY")
    if os.path.isdir(code):
        shutil.copytree(code, drive)
    else:
        os.makedirs(drive)
        shutil.copy(code, os.path.join(drive, "code.py"))
    texts = [a.wait] if a.wait else ["Code done running."]
    texts.append("You are in safe mode")
    results = []
    with Board.launch(
        program,
        drive,
        board_file=a.board_file,
        board=a.board,
        fast_time=a.fast_time,
        code_file=a.code_file,
    ) as b:
        for i in range(a.runs):
            if i:
                b.reload()
            out, seen = _until(b, texts, a.timeout)
            printed = _code_output(out)
            lines = [x for x in printed.splitlines() if x.strip()]
            r = {
                "outcome": "ok",
                "detail": "",
                "last": lines[-1] if lines else "",
                "leaked": [],
                "faults": [],
            }
            if seen == "You are in safe mode" or any("hard fault" in x for x in b.stderr):
                r["outcome"] = "hard fault"
            elif b.process is not None and b.process.poll() is not None:
                r["outcome"] = "program ended"
            elif seen is None:
                r["outcome"] = "not ended in time"
            elif "Traceback (most recent call last)" in printed:
                r["outcome"] = "exception"
                r["detail"] = r["last"]
            if r["outcome"] in ("ok", "exception") and not a.wait:
                left = b.after_run()
                r["leaked"] = left["leaked_pins"]
                r["faults"] = left["faults_fired"]
                # What the program reported, unless it ended in an exception.
                if r["outcome"] == "ok" and left["result"]:
                    r["outcome"] = REPORTED[left["result"]["status"]]
                    r["detail"] = left["result"]["detail"]
            results.append(r)
            if r["outcome"] in ("hard fault", "program ended"):
                break
    return results


def ab(argv):
    """cpapi.py ab PROGRAM_A PROGRAM_B CODE: CODE run --runs times on each program (Ctrl-D
    between runs), and a Markdown table of how the runs ended, for an issue or a PR: the same
    script before and after a change (tools/build-variant.sh builds the second program)."""
    import argparse
    from collections import Counter

    ap = argparse.ArgumentParser(prog="cpapi.py ab", description=ab.__doc__)
    ap.add_argument("program_a")
    ap.add_argument("program_b")
    ap.add_argument("code")
    ap.add_argument("--names", help="the columns' names, A,B (default: the programs' directories)")
    ap.add_argument("--runs", type=int, default=5, help="runs on each program (default 5)")
    ap.add_argument("--board-file")
    ap.add_argument("--board")
    ap.add_argument("--wait", help="a run ends at this console text, not when code.py ends")
    ap.add_argument("--timeout", type=float, default=30, help="seconds for each run (default 30)")
    ap.add_argument("--fast-time", action="store_true")
    ap.add_argument("--code", dest="code_file", help="a file on the drive run in place of code.py")
    try:
        a = ap.parse_args(argv)
    except SystemExit as e:
        return 64 if e.code else 0
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    programs = [a.program_a, a.program_b]
    names = (
        a.names.split(",")
        if a.names
        else [os.path.basename(os.path.dirname(os.path.abspath(p))) for p in programs]
    )
    columns = [_trials(p, a.code, a) for p in programs]

    def counted(results, outcome):
        hits = [r for r in results if r["outcome"] == outcome]
        if not hits:
            return "0/%d" % len(results)
        detail = Counter(r["detail"] for r in hits).most_common(1)[0][0]
        return "%d/%d" % (len(hits), len(results)) + (" `%s`" % detail if detail else "")

    rows = [("runs", [str(len(c)) for c in columns])]
    outcomes = ("ok",) + tuple(REPORTED.values())
    for outcome in outcomes + ("exception", "hard fault", "not ended in time", "program ended"):
        if outcome == "ok" or any(r["outcome"] == outcome for c in columns for r in c):
            rows.append((outcome, [counted(c, outcome) for c in columns]))
    if any(r["leaked"] for c in columns for r in c):
        rows.append(
            (
                "pins left claimed",
                [
                    ", ".join(sorted({p for r in c for p in r["leaked"]})) or "none"
                    for c in columns
                ],
            )
        )
    if any(r["faults"] for c in columns for r in c):
        rows.append(
            (
                "faults fired",
                [_faults(_summed([f for r in c for f in r["faults"]])) or "none" for c in columns],
            )
        )
    rows.append(("each run", [" ".join(r["outcome"].split()[0] for r in c) for c in columns]))
    rows.append(
        (
            "last line",
            [
                "`%s`" % Counter(r["last"] for r in c).most_common(1)[0][0] if c else ""
                for c in columns
            ],
        )
    )
    print("| %s | %s | %s |" % (os.path.basename(a.code), names[0], names[1]))
    print("|---|---|---|")
    for label, cells in rows:
        print("| %s | %s | %s |" % (label, cells[0], cells[1]))
    return 0


def air(argv):
    """cpapi.py air PORT PORT...: the programs started by hand with --api PORT (and the same
    CIRCUITPY_API_TOKEN in the environment) on one air, until Ctrl-C: what one advertises the
    others see, and one connects to another (CLUE to CLUE)."""
    import argparse

    ap = argparse.ArgumentParser(prog="cpapi.py air", description=air.__doc__)
    ap.add_argument("ports", nargs="+", type=int)
    ap.add_argument("--host", default="127.0.0.1")
    try:
        a = ap.parse_args(argv)
    except SystemExit as e:
        return 64 if e.code else 0
    token = os.environ.get("CIRCUITPY_API_TOKEN")
    if not token:
        print("cpapi.py air: set CIRCUITPY_API_TOKEN, as for the programs", file=sys.stderr)
        return 64
    boards = [Board.attach("http://%s:%d/" % (a.host, port), token) for port in a.ports]
    for b in boards:
        try:
            b.info()
        except (ApiError, OSError, http.client.HTTPException) as e:
            print("cpapi.py air: no program at %s: %s" % (b.url, e), file=sys.stderr)
            return 1
    with Air(*boards) as relay:
        print("on one air: %s (Ctrl-C ends it)" % ", ".join(b.url for b in boards))
        try:
            seen = [0] * len(boards)
            while True:
                time.sleep(0.5)
                with relay.lock:
                    for i, events in enumerate(relay.events):
                        for e in events[seen[i] :]:
                            if e["kind"] in ("connected", "disconnected"):
                                print(
                                    "%s %s %s %s"
                                    % (boards[i].url, e["kind"], e["role"], e["peer"])
                                )
                        seen[i] = len(events)
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    commands = {"run": run, "ab": ab, "air": air}
    if sys.argv[1:2] == [] or sys.argv[1] not in commands:
        print(
            "usage: python cpapi.py run PROGRAM CODE [options]  (run --help)\n"
            "       python cpapi.py ab PROGRAM_A PROGRAM_B CODE [options]  (ab --help)\n"
            "       python cpapi.py air PORT PORT...  (air --help)",
            file=sys.stderr,
        )
        sys.exit(64)
    sys.exit(commands[sys.argv[1]](sys.argv[2:]))
