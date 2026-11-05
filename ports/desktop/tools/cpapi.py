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
    ):
        """Start program on drive with --api 0 and a token of its own; stderr is read for the port.
        fast_time: its sleeps return at once and move its clock on (--fast-time). code_file: a
        file on the drive run in place of code.py (--code)."""
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

    def __init__(self, board):
        self.board = board
        self.mtu = 23
        self._notified = {}  # value handle: [values]
        self._services = None
        answer = self._request(bytes([0x02]) + struct.pack("<H", self.MTU), 0x03)
        self.mtu = max(23, min(self.MTU, struct.unpack("<H", answer[1:3])[0]))

    def _send(self, pdu):
        self.board.request("POST", "/api/ble/att", pdu.hex())

    def _take(self, timeout):
        """The PDUs the program sent that are not notifications, waiting up to timeout for one.
        Notifications are kept for notifications(); an indication is confirmed."""
        pdus = self.board.request("GET", "/api/ble/att?wait=%d" % int(timeout * 1000))["pdus"]
        answers = []
        for hexed in pdus:
            pdu = bytes.fromhex(hexed)
            if pdu[:1] in (b"\x1b", b"\x1d"):
                handle = struct.unpack("<H", pdu[1:3])[0]
                self._notified.setdefault(handle, []).append(pdu[3:])
                if pdu[0] == 0x1D:
                    self._send(b"\x1e")
            else:
                answers.append(pdu)
        return answers

    def _request(self, pdu, answer, timeout=5):
        """pdu sent; the program's answer of opcode answer, or AttError. ATT has one request at a
        time, so an answer to another one (late, after a timeout) is dropped."""
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

    def connect(self, timeout=5):
        """The phone joins the program's advertisement: a Peer, its MTU exchanged. Waits up to
        timeout for the program to advertise; any other refusal is raised at once."""
        end = time.time() + timeout
        while True:
            try:
                self.board.request("POST", "/api/ble/connect")
                return Peer(self.board)
            except ApiError as e:
                if "advertises nothing" not in e.message or time.time() > end:
                    raise
                time.sleep(0.05)


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
    fault; 3 when code.py ended in an exception; 64 for wrong arguments."""
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


if __name__ == "__main__":
    if sys.argv[1:2] != ["run"]:
        print("usage: python cpapi.py run PROGRAM CODE [options]  (run --help)", file=sys.stderr)
        sys.exit(64)
    sys.exit(run(sys.argv[2:]))
