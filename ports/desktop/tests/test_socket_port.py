"""A server's port the computer will not give: python3 test_socket_port.py /path/to/circuitpython

A TCP socket whose port is taken, or below 1024 without root on Linux, binds to 8080, 8081 ...
(or the ports after its own when that is 8080 or more) and says so on the console. UDP keeps its
port: a protocol may need it.
"""

import os, re, socket, time
from harness import Run, check, drive


def start(code):
    return Run(["--drive", drive(code)])


def free_pair():
    """A port with the next one free too, both above 8080."""
    for port in range(20080, 30000, 7):
        try:
            for p in (port, port + 1):
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.bind(("", p))
                s.close()
            return port
        except OSError:
            continue
    raise RuntimeError("no free ports")


SERVER = """import wifi, socketpool
pool = socketpool.SocketPool(wifi.radio)
s = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
s.bind(("0.0.0.0", %d))
s.listen(1)
print("listening")
s.settimeout(10)
c, addr = s.accept()
c.send(b"moved")
c.close()
print("served")
"""


def fetch(port):
    for attempt in range(20):
        try:
            c = socket.create_connection(("127.0.0.1", port), timeout=3)
            data = c.recv(16)
            c.close()
            return data
        except OSError:
            time.sleep(0.25)
    return b""


# A taken port: the next one
taken = free_pair()
blocker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
blocker.bind(("", taken))
blocker.listen(1)
r = start(SERVER % taken)
got = fetch(taken + 1) if r.wait_for("listening", 15) else b""
r.stop()
blocker.close()
check(
    "a taken TCP port moves to the next one",
    "port %d is in use, using %d" % (taken, taken + 1) in r.text(),
    r.text(),
)
check("and the server answers there", got == b"moved", got)

# Below 1024 without root on Linux: 8080 or the first free one after it
if os.name != "nt" and os.geteuid() != 0:
    r = start(SERVER % 80)
    r.wait_for("listening", 15)
    m = re.search(r"port 80 is not allowed, using (\d+)", r.text())
    got = fetch(int(m.group(1))) if m else b""
    r.stop()
    check(
        "port 80 without root moves to 8080 or after",
        m is not None and int(m.group(1)) >= 8080,
        r.text(),
    )
    check("and the server answers there", got == b"moved", got)
else:
    print("SKIP port 80 needs no root here")

# UDP keeps its port: the bind fails as it would on a board with the port taken
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.bind(("", taken))
r = start(
    """import wifi, socketpool
pool = socketpool.SocketPool(wifi.radio)
u = pool.socket(pool.AF_INET, pool.SOCK_DGRAM)
try:
    u.bind(("0.0.0.0", %d))
    print("udp bound")
except OSError as e:
    print("udp refused", e)
"""
    % taken
)
r.wait_for("udp ", 15)
r.stop()
udp.close()
check(
    "a taken UDP port is refused, not moved",
    "udp refused" in r.text() and "using" not in r.text(),
    r.text(),
)
print("DONE")
