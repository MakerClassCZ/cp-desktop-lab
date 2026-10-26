"""An MCP server for the CircuitPython program, over its control API (--api); standard library only.

    python3 cp_mcp.py [--program build-linux/circuitpython]

The host starts it and speaks JSON-RPC on stdin and stdout, a message per line. The tools launch
the program (or attach to one already running), run code in its REPL, read and wait for the
console, move the board panel, look at the displays and read and write the drive's files.
"""

import argparse
import base64
import http.client
import json
import os
import re
import shutil
import signal
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpapi  # noqa: E402

VERSIONS = ("2024-11-05", "2025-03-26", "2025-06-18")
MAX_TEXT = 20000  # characters of console or trace returned at most
MAX_WAIT = 60  # seconds a call waits at most, so the host's own timeout does not fire first
MAX_FILE = 1 << 20

# Terminal escapes: OSC (the title bar) up to BEL or ST, CSI, and the two-byte ones.
ESCAPES = re.compile(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b\[[0-?]*[ -/]*[@-~]|\x1b[@-Z\\-_]")


def plain(text):
    """Console text as a reader wants it: no escapes, \\n line ends."""
    return ESCAPES.sub("", text).replace("\r\n", "\n").replace("\r", "")


def tail(text):
    if len(text) <= MAX_TEXT:
        return text
    return (
        "[... %d characters before this are not shown]\n" % (len(text) - MAX_TEXT)
        + text[-MAX_TEXT:]
    )


def clamp_wait(value, default):
    try:
        value = float(default if value is None else value)
    except (TypeError, ValueError):
        value = default
    return max(0.0, min(float(MAX_WAIT), value))


class ToolError(Exception):
    """What the agent is told when a tool cannot do what it asked."""


def _schema(properties=None, required=()):
    return {"type": "object", "properties": properties or {}, "required": list(required)}


STRING = {"type": "string"}
SECONDS = {"type": "number", "description": "seconds, at most %d" % MAX_WAIT}

TOOLS = [
    (
        "launch",
        "Start the CircuitPython program on a CIRCUITPY drive (a directory; code.py runs at once). "
        "board_file is a TOML file of parts (LEDs, buttons, sensors, displays) for the virtual board; board "
        "is virtual (default), ft232h, mcp2221 or gpiochip. Headless unless window is true. One board at a time.",
        _schema(
            {
                "drive": dict(STRING, description="the drive's directory; default CIRCUITPY here"),
                "board_file": STRING,
                "board": STRING,
                "window": {"type": "boolean", "description": "show the board's window"},
                "fast_time": {
                    "type": "boolean",
                    "description": "the program's sleeps return at once and "
                    "move its clock on, for tests that wait; virtual board only, sound silent",
                },
                "args": {
                    "type": "array",
                    "items": STRING,
                    "description": "more command line arguments",
                },
                "program": dict(
                    STRING, description="the circuitpython program, if not the server's"
                ),
            }
        ),
    ),
    (
        "attach",
        "Join a program already started with --api (it printed the url and the token on stderr).",
        _schema({"url": STRING, "token": STRING}, ["url", "token"]),
    ),
    ("stop", "End the program this server launched (an attached one is only let go).", _schema()),
    (
        "info",
        "The program's version, board, drive, process and status (code.py, REPL, Done ...).",
        _schema(),
    ),
    (
        "exec",
        "Run Python code in the REPL, as typed there, and return what it printed and the value of its "
        "last line when that is an expression. Variables last from one exec to the next. While code.py runs this fails unless "
        "interrupt is true, which stops code.py first (its variables are gone then; the REPL is a fresh VM).",
        _schema({"code": STRING, "interrupt": {"type": "boolean"}, "timeout": SECONDS}, ["code"]),
    ),
    (
        "console",
        "The console's output since the last console call. With wait: until that text appears (the "
        "output up to it is returned), at most timeout seconds (default 10). Without: what is there now, or "
        "waits up to timeout for anything. exec and write_file move past what came before them, so a wait "
        "after them sees only what follows. Escape sequences are removed.",
        _schema({"wait": STRING, "timeout": SECONDS}),
    ),
    (
        "write",
        "Type text into the console, as keys: \\r is Enter, \\u0003 is Ctrl-C, \\u0004 is Ctrl-D.",
        _schema({"text": STRING}, ["text"]),
    ),
    ("interrupt", "Ctrl-C: stops code.py (KeyboardInterrupt).", _schema()),
    ("reload", "Ctrl-D: runs code.py again.", _schema()),
    (
        "state",
        "The virtual board's parts as JSON: LEDs, PWM (servo angles), sensors, analog pins, "
        "encoders, NeoPixels, displays and the gamepad buttons held.",
        _schema(),
    ),
    (
        "panel",
        "Move the virtual board's panel: lines such as 'set ENV.temp 24.5', 'press BUTTON_A', "
        "'release BUTTON_A', 'turn ENC 2', 'send IR_RX', 'down Z', 'up Z'. Applied in order before this returns.",
        _schema(
            {
                "commands": {
                    "type": ["string", "array"],
                    "items": STRING,
                    "description": "the lines, as one string or a list",
                }
            },
            ["commands"],
        ),
    ),
    (
        "display",
        "A picture of a display: board.DISPLAY, or a board file's display by name. A small display "
        "is scaled up to at least 256 pixels wide unless scale is given. When code.py ends (or at the REPL) "
        "board.DISPLAY shows the console: a program whose picture should stay keeps running.",
        _schema({"name": STRING, "scale": {"type": "integer", "minimum": 1, "maximum": 8}}),
    ),
    (
        "trace",
        "The bus trace (I2C, SPI, pins, PWM, NeoPixels) since the last trace call; wait and timeout "
        "as for console.",
        _schema({"wait": STRING, "timeout": SECONDS}),
    ),
    (
        "list_files",
        "The files on the drive, with sizes (folders end in /).",
        _schema({"path": dict(STRING, description="a folder on the drive; default its root")}),
    ),
    ("read_file", "A text file on the drive.", _schema({"path": STRING}, ["path"])),
    (
        "write_file",
        "Write a text file on the drive, making its folders. code.py runs again, as on a board "
        "when its drive changes (at the REPL too).",
        _schema({"path": STRING, "content": STRING}, ["path", "content"]),
    ),
]


def _default_program():
    here = os.path.dirname(os.path.abspath(__file__))
    # Beside this file (a release package), else in the port's tree.
    exe = "circuitpython.exe" if os.name == "nt" else "circuitpython"
    names = [exe, os.path.join("..", "build-windows" if os.name == "nt" else "build-linux", exe)]
    for name in names:
        path = os.path.join(here, name)
        if os.path.exists(path):
            return os.path.abspath(path)
    return shutil.which("circuitpython") or "circuitpython"


class Server:
    def __init__(self, program):
        self.program = program
        self.board = None
        self.drive = None  # the drive's path on this computer

    # ---- the board ----

    @property
    def launched(self):
        """This server started the program (an attached one has no process here)."""
        return self.board is not None and self.board.process is not None

    def _ended(self):
        p = self.board.process if self.board else None
        return p is not None and p.poll() is not None

    def _ended_error(self):
        status = self.board.process.poll()
        last = "".join(getattr(self.board, "stderr", [])[-15:]).strip()
        return ToolError(
            "the program ended (status %s); launch it again.%s"
            % (status, "\nIts last messages:\n" + last if last else "")
        )

    def _need(self, alive=True):
        if self.board is None:
            raise ToolError("no board: launch one first (or attach to a running one)")
        if alive and self._ended():
            raise self._ended_error()
        return self.board

    def _api(self, fn, *args, **kw):
        """A call to the board; its failures as the agent should read them."""
        self._need()
        try:
            return fn(*args, **kw)
        except cpapi.ApiError as e:
            raise ToolError("%d %s" % (e.status, e.message))
        except (ConnectionError, OSError, http.client.HTTPException) as e:
            # A program on its way out closes connections before it has ended.
            if self.board.process is not None:
                try:
                    self.board.process.wait(timeout=3)
                except Exception:
                    pass
            if self._ended():
                raise self._ended_error()
            raise ToolError("the program does not answer: %s" % e)

    def close(self):
        if self.launched:
            self.board.close()
        self.board = None
        self.drive = None

    # ---- tools ----

    def t_launch(
        self,
        drive=None,
        board_file=None,
        board=None,
        window=False,
        args=(),
        program=None,
        fast_time=False,
    ):
        if self.board is not None:
            if not self._ended():
                raise ToolError("a board is running: stop it first")
            self.close()
        drive = os.path.abspath(drive or "CIRCUITPY")
        try:
            self.board = cpapi.Board.launch(
                program or self.program,
                drive,
                board_file=os.path.abspath(board_file) if board_file else None,
                board=board,
                headless=not window,
                args=[str(a) for a in args],
                fast_time=bool(fast_time),
            )
        except (OSError, RuntimeError) as e:
            raise ToolError("the program did not start: %s" % e)
        self.drive = drive
        return self.t_info()

    def t_attach(self, url, token):
        if self.board is not None and not self._ended():
            raise ToolError("a board is running: stop it first")
        self.close()
        b = cpapi.Board.attach(url if url.endswith("/") else url + "/", token)
        b.retry = 0
        self.board = b
        try:
            info = self._api(b.info)
        except ToolError:
            self.board = None
            raise
        self.drive = info.get("drive")
        return json.dumps(info, indent=1)

    def t_stop(self):
        self._need(alive=False)
        launched = self.launched
        self.close()
        return "stopped" if launched else "let go; the program keeps running"

    def t_info(self):
        return json.dumps(self._api(self._need().info), indent=1)

    def t_exec(self, code, interrupt=False, timeout=None):
        head, last = split_last_expression(code)
        if head is None:
            return self._exec(code, interrupt, timeout)
        out = self._exec(head, interrupt, timeout, bare=True)
        return (out or "") + (self._exec(last, False, timeout, bare=True) or "") or "(no output)"

    def _exec(self, code, interrupt=False, timeout=None, bare=False):
        b = self._need()
        try:
            r = self._api(b.run, code, bool(interrupt), clamp_wait(timeout, 10) or 1)
        except ToolError as e:
            if str(e).startswith("409"):
                raise ToolError(
                    "code.py is running: pass interrupt: true to stop it and run this in the REPL"
                )
            raise
        # The typed line and the marks are exec's own; the answer carries what the code printed.
        self._skip_console()
        out = plain(r.get("output") or "")
        if not r.get("ok"):
            error = r.get("error") or ""
            raise ToolError(
                tail(
                    out
                    + (
                        "timed out; the code was stopped with Ctrl-C"
                        if error == "timeout"
                        else plain(error)
                    )
                )
            )
        if r.get("value") is not None:
            out += r["value"]
        return tail(out) if out or bare else "(no output)"

    def _stream(self, which, wait, timeout):
        b = self._need()
        attr = "offset" if which == "console" else "trace_offset"
        read = b.console if which == "console" else b.trace
        r = self._api(read, wait=wait or None, timeout=clamp_wait(timeout, 10 if wait else 0))
        data = r["data"]
        if wait and r["matched"]:
            data = cpapi.upto_next(r)
        setattr(b, attr, r["next"])
        text = plain(data)
        if r.get("lost"):
            text = "[earlier output is gone: only the last 64 KiB are kept]\n" + text
        if wait and not r["matched"]:
            text += "\n[%r did not appear]" % wait
        return tail(text) or "(nothing)"

    def t_console(self, wait=None, timeout=None):
        return self._stream("console", wait, timeout)

    def t_trace(self, wait=None, timeout=None):
        return self._stream("trace", wait, timeout)

    def t_write(self, text):
        r = self._api(self._need().write, text)
        return "written: %s bytes" % r.get("written")

    def t_interrupt(self):
        self._api(self._need().interrupt)
        return "Ctrl-C sent"

    def t_reload(self):
        self._api(self._need().reload)
        return "code.py runs again"

    def t_state(self):
        return json.dumps(self._api(self._need().state), indent=1)

    def t_panel(self, commands):
        lines = commands.splitlines() if isinstance(commands, str) else [str(x) for x in commands]
        lines = [x for x in lines if x.strip()]
        if not lines:
            raise ToolError("no commands")
        self._api(self._need().panel, *lines)
        return "applied: " + "; ".join(lines)

    def t_display(self, name=None, scale=None):
        png = self._api(self._need().display, name)
        try:
            png, size, s = repack(png, scale)
        except (ValueError, struct.error, zlib.error) as e:
            raise ToolError("the picture could not be read: %s" % e)
        note = "%s %dx%d" % (name or "DISPLAY", size[0], size[1]) + (
            ", shown %dx" % s if s > 1 else ""
        )
        return [
            {"type": "image", "data": base64.b64encode(png).decode(), "mimeType": "image/png"},
            {"type": "text", "text": note},
        ]

    # ---- the drive ----

    def _root(self):
        self._need(alive=False)
        if not self.drive:
            raise ToolError("the program did not say where its drive is")
        if os.path.isfile(self.drive):
            raise ToolError(
                "the drive is a FAT image: its files can be reached only while the program "
                "is not running (mtools)"
            )
        return os.path.realpath(self.drive)

    def _path(self, path):
        root = self._root()
        full = os.path.realpath(os.path.join(root, path))
        try:
            inside = os.path.normcase(os.path.commonpath([root, full])) == os.path.normcase(root)
        except ValueError:  # on another Windows drive
            inside = False
        if not inside:
            raise ToolError("%s is not on the drive" % path)
        return root, full

    def t_list_files(self, path=""):
        root, top = self._path(path or ".")
        if not os.path.isdir(top):
            raise ToolError("%s is not a folder on the drive" % path)
        lines = []
        for d, dirs, files in os.walk(top):
            dirs[:] = sorted(x for x in dirs if not x.startswith("."))
            rel = os.path.relpath(d, root).replace(os.sep, "/")
            rel = "" if rel == "." else rel + "/"
            if rel:
                lines.append(rel)
            for f in sorted(files):
                try:
                    lines.append("%s%s  %d" % (rel, f, os.path.getsize(os.path.join(d, f))))
                except OSError:
                    pass
            if len(lines) > 1000:
                lines.append("[more not shown]")
                break
        return "\n".join(lines) or "(empty)"

    def t_read_file(self, path):
        _, full = self._path(path)
        if not os.path.isfile(full):
            raise ToolError("no file %s on the drive" % path)
        size = os.path.getsize(full)
        if size > MAX_FILE:
            raise ToolError("%s is %d bytes, more than this reads" % (path, size))
        data = open(full, "rb").read()
        try:
            return data.decode("utf-8")
        except UnicodeDecodeError:
            raise ToolError("%s is not text (%d bytes)" % (path, size))

    def t_write_file(self, path, content):
        _, full = self._path(path)
        if os.path.isdir(full):
            raise ToolError("%s is a folder" % path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(content.encode("utf-8"))
        done = "wrote %s (%d bytes)" % (path, len(content.encode("utf-8")))
        if self.board is None or self._ended():
            return done
        # A console wait after this sees the new run only, not what came before.
        self._skip_console()
        # The drive reloads code.py by itself, except at the REPL, as on a board.
        if self._api(self.board.info).get("status") == "REPL":
            self._api(self.board.reload)
        return done + "; code.py runs again"

    def _skip_console(self):
        try:
            self.board.offset = self.board.console(timeout=0)["end"]
        except (cpapi.ApiError, ConnectionError, OSError, http.client.HTTPException):
            pass

    # ---- the protocol ----

    def call(self, name, arguments):
        """A tools/call result."""
        fn = getattr(self, "t_" + name, None)
        try:
            if fn is None:
                raise ToolError("no tool %s" % name)
            out = fn(**(arguments or {}))
        except ToolError as e:
            return {"content": [{"type": "text", "text": str(e)}], "isError": True}
        except TypeError as e:
            return {
                "content": [{"type": "text", "text": "bad arguments: %s" % e}],
                "isError": True,
            }
        except OSError as e:
            return {"content": [{"type": "text", "text": str(e)}], "isError": True}
        except Exception as e:
            # Whatever a tool did not foresee is the agent's answer, never the server's end.
            return {
                "content": [{"type": "text", "text": "%s: %s" % (type(e).__name__, e)}],
                "isError": True,
            }
        content = out if isinstance(out, list) else [{"type": "text", "text": out}]
        return {"content": content, "isError": False}

    def handle(self, message):
        """The answer to one message, or None for a notification."""
        if (
            not isinstance(message, dict)
            or message.get("jsonrpc") != "2.0"
            or "method" not in message
        ):
            return _error(
                message.get("id") if isinstance(message, dict) else None, -32600, "invalid request"
            )
        if "id" not in message:
            return None
        method, params, mid = message["method"], message.get("params") or {}, message["id"]
        if not isinstance(params, dict):
            return _error(mid, -32602, "params must be an object")
        if method == "initialize":
            asked = params.get("protocolVersion")
            return _result(
                mid,
                {
                    "protocolVersion": asked if asked in VERSIONS else VERSIONS[-1],
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "circuitpython", "version": "1.0"},
                },
            )
        if method == "ping":
            return _result(mid, {})
        if method == "tools/list":
            return _result(
                mid,
                {"tools": [{"name": n, "description": d, "inputSchema": s} for n, d, s in TOOLS]},
            )
        if method == "tools/call":
            name = params.get("name")
            if name not in {t[0] for t in TOOLS}:
                return _error(mid, -32602, "no tool %s" % name)
            return _result(mid, self.call(name, params.get("arguments")))
        return _error(mid, -32601, "no method %s" % method)


def _result(mid, result):
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def _error(mid, code, message):
    return {"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}}


def split_last_expression(code):
    """Statements ending in an expression as (the statements, the expression), so its value comes
    back as in a notebook; (None, None) otherwise, or when this Python cannot parse the code."""
    import ast

    try:
        body = ast.parse(code).body
    except (SyntaxError, ValueError):
        return None, None
    if len(body) < 2 or not isinstance(body[-1], ast.Expr):
        return None, None
    lines = code.splitlines(True)
    start = body[-1].lineno - 1
    if body[-1].col_offset != 0 or body[-2].end_lineno > start:
        return None, None  # on a line shared with the statement before it
    return "".join(lines[:start]), "".join(lines[start:])


def repack(png, scale=None):
    """The port's PNG (stored deflate) compressed, a small one scaled up by a whole factor:
    (png, (width, height), factor)."""
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, ihdr = 8, b"", None
    while pos < len(png):
        n, kind = struct.unpack(">I4s", png[pos : pos + 8])
        if kind == b"IHDR":
            ihdr = png[pos + 8 : pos + 8 + n]
        elif kind == b"IDAT":
            idat += png[pos + 8 : pos + 8 + n]
        pos += 12 + n
    w, h, depth, color = struct.unpack(">IIBB", ihdr[:10])
    raw = zlib.decompress(idat)
    s = int(scale) if scale else (-(-256 // w) if w < 200 else 1)
    s = max(1, min(8, s))
    row = 1 + w * 3
    # Scaled only when the rows are unfiltered 8-bit RGB, as the port writes them.
    if s > 1 and depth == 8 and color == 2 and all(raw[y * row] == 0 for y in range(h)):
        out = bytearray()
        for y in range(h):
            line = raw[y * row + 1 : (y + 1) * row]
            wide = b"".join(line[x : x + 3] * s for x in range(0, len(line), 3))
            out += (b"\x00" + wide) * s
        raw, w, h = bytes(out), w * s, h * s
        ihdr = struct.pack(">IIBB", w, h, depth, color) + ihdr[10:]
    else:
        s = 1

    def chunk(kind, body):
        return (
            struct.pack(">I", len(body))
            + kind
            + body
            + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
        )

    return (
        png[:8]
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b""),
        (w, h),
        s,
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--program",
        default=os.environ.get("CIRCUITPY_PROGRAM") or _default_program(),
        help="the circuitpython program to launch",
    )
    server = Server(ap.parse_args().program)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    out = sys.stdout.buffer
    try:
        for line in sys.stdin.buffer:
            if not line.strip():
                continue
            try:
                message = json.loads(line)
            except ValueError:
                answer = _error(None, -32700, "not JSON")
            else:
                try:
                    answer = server.handle(message)
                except Exception as e:
                    answer = _error(
                        message.get("id") if isinstance(message, dict) else None,
                        -32603,
                        "%s: %s" % (type(e).__name__, e),
                    )
            if answer is not None:
                out.write(json.dumps(answer).encode() + b"\n")
                out.flush()
    finally:
        server.close()


if __name__ == "__main__":
    main()
