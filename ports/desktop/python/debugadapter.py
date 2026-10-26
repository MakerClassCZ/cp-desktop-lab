# This file is part of the CircuitPython project: https://circuitpython.org
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
#
# SPDX-License-Identifier: MIT

"""A debug adapter inside CircuitPython on a host.

An editor speaking the Debug Adapter Protocol (VS Code) connects over TCP, and the program
runs under sys.settrace: breakpoints, stepping, the call stack, variables and expressions.
``circuitpython --debug PORT`` imports this file before code.py and calls start(); it waits for
the editor, takes its configuration and installs the trace function.
"""

import json
import sys
import time

import atexit
import hostnetwork
import socketpool
import supervisor

_OWN_FILES = ("debugadapter.py", "<debug>")
# Ways a program stops that are no error to show.
_NOT_ERRORS = ("KeyboardInterrupt", "SystemExit", "GeneratorExit", "ReloadException")
_POLL_NS = 50000000  # how often the running program looks for editor requests
_REPR_MAX = 200
_PRIMITIVE = (int, float, str, bool, bytes, type(None))
_THREAD = {"id": 1, "name": "main"}


def _basename(path):
    return path.replace("\\", "/").rsplit("/", 1)[-1]


def _short(value):
    try:
        text = repr(value)
    except Exception as e:
        text = "<repr failed: %s>" % e
    if len(text) > _REPR_MAX:
        text = text[:_REPR_MAX] + "..."
    return text


class Adapter:
    def __init__(self, port, drive):
        self.port = port
        self.drive = drive.replace("\\", "/").rstrip("/")
        self.pool = socketpool.SocketPool(hostnetwork.HostNetwork())
        self.sock = None
        self.rx = bytearray()
        self.chunk = bytearray(4096)
        self.seq = 1
        self.connected = False
        self.configured = False
        self.breakpoints = {}  # "/lib/x.py" -> set of lines
        self.break_on_exception = False
        self.mode = "run"  # run, step, next, out
        self.step_depth = 0
        self.pause_requested = False
        self.terminate_requested = False
        self.resumed = False
        self.frame = None
        self.frames = []
        self.handles = []
        self.last_poll = 0
        self.raised = None  # (exception, "/x.py", line) where the latest one was raised
        self.uncaught = False  # it reached code.py's own frame and nothing handled it there

    # -- paths -------------------------------------------------------------

    def rel_path(self, filename):
        """A path on the drive, as the VM names it, as "/lib/x.py"."""
        return "/" + filename.replace("\\", "/").lstrip("/")

    def editor_to_rel(self, path):
        path = path.replace("\\", "/")
        if path.lower().startswith(self.drive.lower() + "/"):
            return path[len(self.drive) :]
        return "/" + _basename(path)

    def rel_to_editor(self, rel):
        path = self.drive + rel
        if len(self.drive) > 1 and self.drive[1] == ":":
            path = path.replace("/", "\\")  # a Windows path, as the editor spells it
        return path

    # -- the wire ------------------------------------------------------------

    def wait_for_client(self):
        listener = self.pool.socket(self.pool.AF_INET, self.pool.SOCK_STREAM)
        listener.bind(("127.0.0.1", self.port))
        # The editor's probes and its connection can arrive together while one is being read.
        listener.listen(4)
        listener.settimeout(None)
        print("debug adapter: waiting for the editor on port %d" % self.port)
        while not self.configured:
            sock, _ = listener.accept()
            self.sock = sock
            self.connected = True
            self.rx = bytearray()
            # The editor's first request, or a probe that only checks the port is open.
            message = self.read_message(True, 5.0)
            if message is None:
                self.close()
                continue
            self.handle(message)
            while self.connected and not self.configured:
                message = self.read_message(True, 0.2)
                if message is not None:
                    self.handle(message)
        listener.close()
        print("debug adapter: editor connected")

    def close(self):
        self.connected = False
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def _fill(self, block, timeout):
        if not self.connected:
            return False
        self.sock.settimeout(timeout if block else 0)
        try:
            n = self.sock.recv_into(self.chunk)
        except OSError:
            return False
        if n == 0:
            self.close()
            return False
        self.rx.extend(memoryview(self.chunk)[:n])
        return True

    def _take_message(self):
        head = self.rx.find(b"\r\n\r\n")
        if head < 0:
            return None
        length = 0
        for line in bytes(self.rx[:head]).decode().split("\r\n"):
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1].strip())
        start = head + 4
        if len(self.rx) < start + length:
            return None
        body = bytes(self.rx[start : start + length])
        self.rx = self.rx[start + length :]
        return json.loads(body)

    def read_message(self, block, timeout=0.2):
        """The next request, or None when none arrived within the wait."""
        while True:
            message = self._take_message()
            if message is not None:
                return message
            if not self._fill(block, timeout):
                return None

    def send(self, message):
        if not self.connected:
            return
        message["seq"] = self.seq
        self.seq += 1
        body = json.dumps(message).encode()
        data = ("Content-Length: %d\r\n\r\n" % len(body)).encode() + body
        self.sock.settimeout(None)
        while data:
            try:
                n = self.sock.send(data)
            except OSError:
                self.close()
                return
            data = data[n:]

    def event(self, name, body=None):
        self.send({"type": "event", "event": name, "body": body or {}})

    def respond(self, request, body=None, error=None):
        message = {
            "type": "response",
            "request_seq": request["seq"],
            "command": request["command"],
            "success": error is None,
        }
        if error is not None:
            message["message"] = error
        else:
            message["body"] = body or {}
        self.send(message)

    # -- requests ------------------------------------------------------------

    def handle(self, request):
        if request.get("type") != "request":
            return
        command = request.get("command")
        args = request.get("arguments") or {}
        handler = getattr(self, "on_" + command, None)
        if handler is None:
            self.respond(request)
            return
        try:
            handler(request, args)
        except Exception as e:
            self.respond(request, error="%s: %s" % (type(e).__name__, e))

    def on_initialize(self, request, args):
        self.respond(
            request,
            {
                "supportsConfigurationDoneRequest": True,
                "supportsEvaluateForHovers": True,
                "supportsTerminateRequest": True,
                "exceptionBreakpointFilters": [
                    {"filter": "all", "label": "All exceptions", "default": False},
                ],
            },
        )
        self.event("initialized")

    def on_setBreakpoints(self, request, args):
        rel = self.editor_to_rel(args["source"]["path"])
        lines = [b["line"] for b in args.get("breakpoints") or []]
        if lines:
            self.breakpoints[rel] = set(lines)
        else:
            self.breakpoints.pop(rel, None)
        self.respond(
            request, {"breakpoints": [{"verified": True, "line": line} for line in lines]}
        )

    def on_setExceptionBreakpoints(self, request, args):
        self.break_on_exception = "all" in (args.get("filters") or [])
        self.respond(request)

    def on_configurationDone(self, request, args):
        self.configured = True
        self.respond(request)

    def on_threads(self, request, args):
        self.respond(request, {"threads": [_THREAD]})

    def on_pause(self, request, args):
        self.pause_requested = True
        self.respond(request)

    def on_disconnect(self, request, args):
        self.respond(request)
        sys.settrace(None)
        self.close()
        self.resumed = True

    def on_terminate(self, request, args):
        # Stop in the editor reloads the program, which then waits for the editor again, so the
        # next start connects to it.
        self.respond(request)
        self.terminate_requested = True
        self.resumed = True
        if self.frame is None:
            supervisor.reload()

    def on_continue(self, request, args):
        self.respond(request, {"allThreadsContinued": True})
        self.resumed = True

    def on_next(self, request, args):
        self.respond(request)
        self.mode = "next"
        self.step_depth = self.depth(self.frame)
        self.resumed = True

    def on_stepIn(self, request, args):
        self.respond(request)
        self.mode = "step"
        self.resumed = True

    def on_stepOut(self, request, args):
        self.respond(request)
        self.mode = "out"
        self.step_depth = self.depth(self.frame)
        self.resumed = True

    def on_stackTrace(self, request, args):
        if self.frame is None:
            self.respond(request, error="not stopped")
            return
        self.frames = []
        frame = self.frame
        while frame is not None:
            if _basename(frame.f_code.co_filename) not in _OWN_FILES:
                self.frames.append(frame)
            frame = frame.f_back
        frames = []
        for i, frame in enumerate(self.frames):
            rel = self.rel_path(frame.f_code.co_filename)
            frames.append(
                {
                    "id": i + 1,
                    "name": frame.f_code.co_name,
                    "source": {"name": _basename(rel), "path": self.rel_to_editor(rel)},
                    "line": frame.f_lineno,
                    "column": 1,
                }
            )
        self.respond(request, {"stackFrames": frames, "totalFrames": len(frames)})

    def frame_for(self, frame_id):
        if 1 <= frame_id <= len(self.frames):
            return self.frames[frame_id - 1]
        return self.frame

    def namespaces(self, frame):
        """The locals and globals of frame; a module's locals are its globals."""
        if frame.f_code.co_name == "<module>":
            return frame.f_globals, frame.f_globals
        return frame.f_locals, frame.f_globals

    def on_scopes(self, request, args):
        frame = self.frame_for(args.get("frameId", 1))
        locals_, globals_ = self.namespaces(frame)
        self.respond(
            request,
            {
                "scopes": [
                    {
                        "name": "Locals",
                        "variablesReference": self.handle_for(locals_),
                        "expensive": False,
                    },
                    {
                        "name": "Globals",
                        "variablesReference": self.handle_for(globals_),
                        "expensive": True,
                    },
                ]
            },
        )

    def handle_for(self, value):
        self.handles.append(value)
        return len(self.handles)

    def variable(self, name, value):
        expandable = not isinstance(value, _PRIMITIVE) and not callable(value)
        return {
            "name": str(name),
            "value": _short(value),
            "type": type(value).__name__,
            "variablesReference": self.handle_for(value) if expandable else 0,
        }

    def on_variables(self, request, args):
        ref = args.get("variablesReference", 0)
        if not 1 <= ref <= len(self.handles):
            self.respond(request, error="unknown variable")
            return
        container = self.handles[ref - 1]
        variables = []
        if isinstance(container, dict):
            for key in sorted(container, key=str):
                if isinstance(key, str) and key.startswith("__"):
                    continue
                variables.append(self.variable(key, container[key]))
        elif isinstance(container, (list, tuple, set, frozenset)):
            for i, item in enumerate(container):
                variables.append(self.variable(i, item))
        else:
            for name in dir(container):
                if name.startswith("_"):
                    continue
                try:
                    attr = getattr(container, name)
                except Exception:
                    continue
                if not callable(attr):
                    variables.append(self.variable(name, attr))
        self.respond(request, {"variables": variables})

    def on_evaluate(self, request, args):
        if self.frame is None:
            self.respond(request, error="not stopped")
            return
        frame = self.frame_for(args.get("frameId", 1))
        locals_, globals_ = self.namespaces(frame)
        expression = args.get("expression", "")
        try:
            value = eval(expression, globals_, locals_)
        except Exception as e:
            self.respond(request, error="%s: %s" % (type(e).__name__, e))
            return
        self.respond(
            request,
            {
                "result": _short(value),
                "type": type(value).__name__,
                "variablesReference": self.handle_for(value)
                if not isinstance(value, _PRIMITIVE)
                else 0,
            },
        )

    # -- tracing -------------------------------------------------------------

    def depth(self, frame):
        n = 0
        while frame is not None:
            n += 1
            frame = frame.f_back
        return n

    def poll(self):
        """Requests that arrive while the program runs: breakpoints, pause, disconnect."""
        while self.connected:
            message = self.read_message(False)
            if message is None:
                return
            self.handle(message)

    def stop_reason(self, frame):
        if self.pause_requested:
            return "pause"
        if self.mode == "step":
            return "step"
        if self.mode != "run":
            depth = self.depth(frame)
            if self.mode == "next" and depth <= self.step_depth:
                return "step"
            if self.mode == "out" and depth < self.step_depth:
                return "step"
        lines = self.breakpoints.get(self.rel_path(frame.f_code.co_filename))
        if lines and frame.f_lineno in lines:
            return "breakpoint"
        return None

    def stop(self, frame, reason, exc):
        self.mode = "run"
        self.pause_requested = False
        self.frame = frame
        self.frames = []
        self.handles = []
        body = {"reason": reason, "threadId": 1, "allThreadsStopped": True}
        if exc is not None:
            body["description"] = _short(exc)
        self.event("stopped", body)
        self.resumed = False
        while self.connected and not self.resumed:
            message = self.read_message(True, 0.2)
            if message is not None:
                self.handle(message)
        self.frame = None
        self.frames = []
        self.handles = []
        if self.terminate_requested:
            supervisor.reload()

    def trace(self, frame, event, arg):
        if _basename(frame.f_code.co_filename) in _OWN_FILES:
            return None
        if event == "line":
            now = time.monotonic_ns()
            if now - self.last_poll > _POLL_NS:
                self.last_poll = now
                self.poll()
            if frame.f_back is None:
                self.uncaught = False
            reason = self.stop_reason(frame)
            if reason is not None:
                self.stop(frame, reason, None)
        elif event == "exception":
            self.note_exception(frame, arg)
            if self.break_on_exception:
                self.stop(frame, "exception", arg)
        return self.trace

    def note_exception(self, frame, arg):
        # MicroPython's event carries no argument; the exception is the one being handled.
        exc = sys.exc_info()[1] if arg is None else arg[1] if isinstance(arg, tuple) else arg
        if type(exc).__name__ in _NOT_ERRORS:
            return
        if self.raised is None or self.raised[0] is not exc:
            self.raised = (exc, self.rel_path(frame.f_code.co_filename), frame.f_lineno)
        if frame.f_back is None:
            self.uncaught = True

    def finish(self):
        # An exception that ended code.py, for the editor's Problems panel.
        if self.connected and self.uncaught and self.raised is not None:
            exc, rel, line = self.raised
            self.event(
                "circuitpython.uncaught",
                {
                    "path": self.rel_to_editor(rel),
                    "line": line,
                    "text": "%s: %s" % (type(exc).__name__, exc)
                    if str(exc)
                    else type(exc).__name__,
                },
            )
        if self.connected:
            self.event("exited", {"exitCode": 0})
            self.event("terminated")
            self.close()


def start(port, drive):
    adapter = Adapter(port, drive)
    adapter.wait_for_client()
    atexit.register(adapter.finish)
    sys.settrace(adapter.trace)
    return adapter
