# A Debug Adapter Protocol client that drives circuitpython --debug like an editor would:
# breakpoints (one on the first line of code.py), stack, scopes, locals, evaluate, step into a
# library file, step out, step over, continue, and the end of the program.
import json, os, shutil, socket, time
from harness import Run, T, check
D = os.path.join(T, "DAPDRIVE")
PORT = 5699
shutil.rmtree(D, ignore_errors=True)
os.makedirs(os.path.join(D, "lib"))
open(os.path.join(D, "code.py"), "w").write('''import helper
counter = 0

def compute(x):
    y = x * 2
    z = helper.double(y)
    return z + 1

for i in range(3):
    counter += compute(i)
print("result", counter)
''')
open(os.path.join(D, "lib", "helper.py"), "w").write('''def double(v):
    w = v + v
    return w
''')

p = Run(["--drive", D, "--debug", str(PORT)])
text = p.text

UNCAUGHT = """def caught():
    raise ValueError("handled")
try:
    caught()
except ValueError:
    pass
def boom():
    return 1 / 0
boom()
"""

class Client:
    def __init__(self):
        self.seq = 1
        self.buf = b""
        self.events = []
        self.sock = None
    def connect(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=2)
                self.sock.settimeout(20)
                return True
            except OSError:
                time.sleep(0.3)
        return False
    def send(self, command, arguments=None):
        msg = {"seq": self.seq, "type": "request", "command": command, "arguments": arguments or {}}
        self.seq += 1
        body = json.dumps(msg).encode()
        self.sock.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        return msg["seq"]
    def read(self):
        while True:
            head = self.buf.find(b"\r\n\r\n")
            if head >= 0:
                length = int([l for l in self.buf[:head].decode().split("\r\n") if l.lower().startswith("content-length")][0].split(":")[1])
                if len(self.buf) >= head + 4 + length:
                    body = self.buf[head + 4:head + 4 + length]
                    self.buf = self.buf[head + 4 + length:]
                    return json.loads(body)
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("adapter closed the connection")
            self.buf += chunk
    def request(self, command, arguments=None):
        seq = self.send(command, arguments)
        while True:
            msg = self.read()
            if msg["type"] == "event":
                self.events.append(msg)
            elif msg["type"] == "response" and msg["request_seq"] == seq:
                return msg
    def wait_event(self, name, timeout=20):
        for e in list(self.events):
            if e["event"] == name:
                self.events.remove(e)
                return e
        end = time.time() + timeout
        while time.time() < end:
            msg = self.read()
            if msg["type"] == "event":
                if msg["event"] == name:
                    return msg
                self.events.append(msg)
        return None

c = Client()
try:
    check("program waits for the editor", p.wait_for("waiting for the editor", 60), text()[-300:])
    check("connects", c.connect(20))
    r = c.request("initialize", {"clientID": "test", "adapterID": "circuitpython"})
    check("initialize", r["success"] and r["body"].get("supportsConfigurationDoneRequest"))
    check("initialized event", c.wait_event("initialized") is not None)
    r = c.request("setBreakpoints", {"source": {"path": os.path.join(D, "code.py")}, "breakpoints": [{"line": 1}, {"line": 6}]})
    check("breakpoints accepted", [b["line"] for b in r["body"]["breakpoints"]] == [1, 6])
    c.request("setExceptionBreakpoints", {"filters": []})
    c.request("configurationDone")
    e = c.wait_event("stopped")
    check("stops on the first line of code.py", e is not None and e["body"]["reason"] == "breakpoint", e)
    st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
    check("module frame at line 1", st and st[0]["name"] == "<module>" and st[0]["line"] == 1 and st[0]["source"]["path"].replace("\\", "/").endswith("/code.py"), st)
    c.request("continue", {"threadId": 1})
    e = c.wait_event("stopped")
    check("stops at the breakpoint in compute", e is not None and e["body"]["reason"] == "breakpoint", e)
    st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
    check("stack: compute line 6 over <module> line 10", len(st) == 2 and st[0]["name"] == "compute" and st[0]["line"] == 6 and st[1]["name"] == "<module>" and st[1]["line"] == 10, st)
    scopes = c.request("scopes", {"frameId": 1})["body"]["scopes"]
    locals_ref = [s for s in scopes if s["name"] == "Locals"][0]["variablesReference"]
    globals_ref = [s for s in scopes if s["name"] == "Globals"][0]["variablesReference"]
    v = {x["name"]: x["value"] for x in c.request("variables", {"variablesReference": locals_ref})["body"]["variables"]}
    check("locals x=0 y=0", v.get("x") == "0" and v.get("y") == "0", v)
    g = {x["name"]: x["value"] for x in c.request("variables", {"variablesReference": globals_ref})["body"]["variables"]}
    check("globals counter=0 and i=0", g.get("counter") == "0" and g.get("i") == "0", g)
    r = c.request("evaluate", {"expression": "y + 40", "frameId": 1, "context": "watch"})
    check("evaluate in the frame", r["success"] and r["body"]["result"] == "40", r)
    c.request("stepIn", {"threadId": 1})
    e = c.wait_event("stopped")
    st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
    check("step in lands in lib/helper.py double()", e["body"]["reason"] == "step" and st[0]["name"] == "double" and st[0]["source"]["path"].replace("\\", "/").endswith("/lib/helper.py"), st)
    c.request("stepOut", {"threadId": 1})
    e = c.wait_event("stopped")
    st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
    check("step out returns to compute line 7", st[0]["name"] == "compute" and st[0]["line"] == 7, st)
    c.request("next", {"threadId": 1})
    e = c.wait_event("stopped")
    st = c.request("stackTrace", {"threadId": 1})["body"]["stackFrames"]
    check("step over the return reaches <module>", st[0]["name"] == "<module>" and st[0]["line"] in (9, 10), st)
    c.request("continue", {"threadId": 1})
    e = c.wait_event("stopped")
    scopes = c.request("scopes", {"frameId": 1})["body"]["scopes"]
    v = {x["name"]: x["value"] for x in c.request("variables", {"variablesReference": scopes[0]["variablesReference"]})["body"]["variables"]}
    check("second hit: x=1 y=2", v.get("x") == "1" and v.get("y") == "2", v)
    c.request("setBreakpoints", {"source": {"path": os.path.join(D, "code.py")}, "breakpoints": []})
    c.request("continue", {"threadId": 1})
    e = c.wait_event("terminated", 30)
    check("program ends with terminated event", e is not None)
    # Stop in the editor: the program reloads and waits for the editor again.
    check("after the end the program is not waiting (code.py finished)", not p.wait_for("waiting for the editor", 3, 2))
    check("program output result 15", p.wait_for("result 15", 10), text()[-200:])
    check("a clean end reports no uncaught exception", not any(m.get("event") == "circuitpython.uncaught" for m in c.events))
    # A saved file reloads the program, which waits again: a probe that hangs up, then the editor.
    # The new code.py handles one exception and not the next.
    open(os.path.join(D, "code.py"), "w").write(UNCAUGHT)
    check("a saved file: the program waits for the editor again", p.wait_for("waiting for the editor", 30, 2))
    # The extension's reconnect: its probe, the launch's probe and the editor, back to back while
    # the adapter still reads the first. One attempt each: a full backlog refuses on Windows.
    probe = Client()
    check("a saved file: a probe connects", probe.connect(10))
    time.sleep(0.5)
    queued = []
    for name in ("second probe", "editor"):
        try:
            queued.append(socket.create_connection(("127.0.0.1", PORT), timeout=2))
        except OSError as e:
            check("a saved file: the %s is queued while the first probe is open" % name, False, e)
    check("a saved file: two more connections queue behind an open probe", len(queued) == 2)
    probe.sock.close()
    if len(queued) == 2:
        queued[0].close()
        c3 = Client()
        c3.sock = queued[1]
        c3.sock.settimeout(20)
        r = c3.request("initialize", {"clientID": "test", "adapterID": "circuitpython"})
        check("a saved file: the editor connects after the probes", r["success"], r)
        c3.request("setExceptionBreakpoints", {"filters": []})
        c3.request("configurationDone")
        e = c3.wait_event("circuitpython.uncaught", 20)
        b = e["body"] if e else {}
        check("an uncaught exception: where it was raised and what it says",
              b.get("path", "").replace("\\", "/").endswith("/code.py") and b.get("line") == 8
              and b.get("text") == "ZeroDivisionError: division by zero", e)
except Exception as ex:
    check("protocol run (%r)" % ex, False)
finally:
    p.stop()
print("--- tail ---"); print(text().replace("\x1b", "^[")[-500:])
