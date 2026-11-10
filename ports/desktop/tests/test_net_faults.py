"""The network's faults: python3 test_net_faults.py /path/to/circuitpython

"fault wifi.connect|wifi.link|net.dns|net.socket|net.connect|net.send|net.recv ...": wifi's
sockets on the computer fail as a board's do, with its messages and errors. --sockets N and
--profile limit how many sockets the program may open.
"""

import os, socketserver, sys, threading
from harness import T, check, fetch_libs, launch

GOT = []  # what the sink received, a connection's bytes each


class Peer(socketserver.BaseRequestHandler):
    """Sends ten bytes, then waits for the end."""

    def handle(self):
        self.request.sendall(b"0123456789")
        try:
            while self.request.recv(1024):
                pass
        except OSError:
            pass


class Sink(socketserver.BaseRequestHandler):
    """Takes what comes until the end. It sends nothing: a socket closed with bytes unread ends
    with a reset, and on Windows a reset loses what the peer had not read yet."""

    def handle(self):
        data = b""
        try:
            while chunk := self.request.recv(1024):
                data += chunk
        except OSError:
            pass
        GOT.append(data)


def serve(handler):
    server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


server, sink = serve(Peer), serve(Sink)
PORT, SINK = server.server_address[1], sink.server_address[1]

COMMON = """import errno, lab, socketpool, time, wifi
pool = socketpool.SocketPool(wifi.radio)
PORT, SINK = %d, %d
NAMES = {getattr(errno, n): n for n in ("ECONNREFUSED", "ECONNRESET", "ETIMEDOUT", "EHOSTUNREACH", "EIO")}

def attempt(f):
    try:
        return f()
    except Exception as e:
        if type(e) is OSError:
            return NAMES.get(e.errno, e.errno)
        return type(e).__name__ + (": " + str(e) if type(e).__name__ != "gaierror" else "")

def conn(timeout=None, port=PORT):
    s = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
    if timeout is not None:
        s.settimeout(timeout)
    s.connect(("127.0.0.1", port))
    return s

def opened():
    s = conn()
    s.close()
    return "ok"

buf = bytearray(20)
""" % (PORT, SINK)

PROGRAM = (
    COMMON
    + """
lab.panel("fault net.connect refuse port=%d once" % PORT)
print("connect", attempt(opened), attempt(opened))

lab.panel("fault net.dns fail host=localhost")
print("dns", attempt(lambda: pool.getaddrinfo("localhost", PORT) and "ok"),
    attempt(lambda: pool.getaddrinfo("127.0.0.1", PORT) and "ok"))
lab.panel("fault clear net.dns")

lab.panel("fault net.socket full once")
print("socket", attempt(lambda: pool.socket().close() or "ok"), attempt(lambda: pool.socket().close() or "ok"))

lab.panel("fault net.send cut 5 once")
s = conn(5, SINK)
print("send cut", attempt(lambda: s.send(b"hello world")), attempt(lambda: s.send(b"more")))
s.close()

lab.panel("fault net.recv cut 3 once")
s = conn(5)
n = s.recv_into(buf)
print("recv cut", bytes(buf[:n]), attempt(lambda: s.recv_into(buf)))
s.close()

lab.panel("fault net.recv reset once")
s = conn(5)
print("recv reset", attempt(lambda: s.recv_into(buf)))
s.close()

lab.panel("fault net.recv stall once")
s = conn(1)
t = time.monotonic()
print("recv stall", attempt(lambda: s.recv_into(buf)), round(time.monotonic() - t))
s.close()

lab.panel("fault net.connect timeout once")
t = time.monotonic()
print("connect timeout", attempt(lambda: conn(2)), round(time.monotonic() - t))

lab.panel("fault wifi.connect auth ssid=Lab once", "fault wifi.connect noap ssid=Gone")
print("wifi", attempt(lambda: wifi.radio.connect("Lab", "secret12")), "|",
    attempt(lambda: wifi.radio.connect("Gone", "secret12")), "|", attempt(lambda: wifi.radio.connect("Lab", "secret12")))

s = conn(5)
lab.panel("fault wifi.link drop once")
print("link", attempt(lambda: s.send(b"x")), wifi.radio.connected,
    attempt(lambda: pool.getaddrinfo("localhost", PORT) and "ok"))
s.close()
wifi.radio.connect("Lab", "secret12")
print("relinked", wifi.radio.connected, attempt(opened))
print("fired", lab.fired())
"""
)

with launch(PROGRAM, fast_time=True) as b:
    out = b.wait("fired", timeout=60)
    out += b.wait("\n", timeout=5)
    trace = b.trace(since=0)["data"]


def line(name):
    """The program's line that starts with name (after the title the console sets)."""
    lines = (x.rpartition("\x1b\\")[2] for x in out.splitlines())
    return next((x for x in lines if x.startswith(name + " ")), "")


check(
    "net.connect refuse port= once: ECONNREFUSED, then it connects",
    line("connect") == "connect ECONNREFUSED ok",
    out,
)
check("net.dns fail host=: that name only, gaierror", line("dns") == "dns gaierror ok", out)
check(
    "net.socket full: the board's RuntimeError",
    line("socket") == "socket RuntimeError: Out of sockets ok",
    out,
)
check(
    "net.send cut 5: five bytes go, then the connection is reset",
    line("send cut") == "send cut 5 ECONNRESET" and b"hello" in GOT,
    (out, GOT),
)
check("net.recv cut 3: three bytes, then the end", line("recv cut") == "recv cut b'012' 0", out)
check("net.recv reset: ECONNRESET", line("recv reset") == "recv reset ECONNRESET", out)
check(
    "net.recv stall: nothing comes; the socket's timeout on the program's clock",
    line("recv stall") == "recv stall ETIMEDOUT 1",
    out,
)
check(
    "net.connect timeout: after the socket's timeout, ETIMEDOUT",
    line("connect timeout") == "connect timeout ETIMEDOUT 2",
    out,
)
check(
    "wifi.connect auth / noap ssid=: the board's ConnectionError messages",
    line("wifi")
    == "wifi ConnectionError: Authentication failure | ConnectionError: No network with that ssid | None",
    out,
)
check(
    "wifi.link drop: the connection reset, the radio off the network, lookups fail",
    line("link") == "link ECONNRESET False gaierror",
    out,
)
check("connecting again brings the link back", line("relinked") == "relinked True ok", out)
check(
    "the trace says where each fault fired",
    "FAULT net.connect 127.0.0.1:%d: refuse" % PORT in trace
    and "FAULT net.dns localhost: fail" in trace
    and "FAULT wifi.connect Lab: auth" in trace
    and "FAULT wifi.link: drop" in trace,
    trace[-3000:],
)

# The socket limit: --sockets, and a board profile's.
LIMIT = (
    COMMON
    + """
held = []
while attempt(lambda: held.append(pool.socket()) or "ok") == "ok":
    pass
print("opened", len(held), attempt(lambda: pool.socket()))
held.pop().close()
print("after a close", attempt(lambda: pool.socket().close() or "ok"))
"""
)
for args, n in ((("--sockets", "2"), 2), (("--profile", "esp32s3"), 8)):
    with launch(LIMIT, args=args) as b:
        out = b.wait("after a close", timeout=30) + b.wait("\n", timeout=5)
    check(
        "%s: %d sockets, then Out of sockets until one is closed" % (" ".join(args), n),
        "opened %d RuntimeError: Out of sockets" % n in out and "after a close ok" in out,
        out,
    )

# A rule from outside waits for the program's network call: asking wifi.radio.connected (as the
# supervisor asks for the address) does not fire it.
OUTSIDE = (
    COMMON
    + """
wifi.radio.connect("Lab", "secret12")
print("before", wifi.radio.connected, lab.fired())
print("after", attempt(lambda: conn(5).close()), wifi.radio.connected, lab.fired())
"""
)
with launch(OUTSIDE) as b:
    b.wait("after", timeout=30)
    b.panel("fault wifi.link drop once")
    b.reload()
    b.wait("before", timeout=30)
    out = "before" + b.wait("Code done running.", timeout=30)
check(
    "wifi.link drop from outside: across a reload, fired by the program's first network call",
    "before True []" in out and "after gaierror False [('wifi.link', 'drop', 1)]" in out,
    out,
)

# The same points on an AirLift: what nina-fw reports, raised as adafruit_esp32spi raises it.
BOARDS = next(
    p
    for p in (os.path.join(T, "board_files"), os.path.join(T, "..", "boards", "files"))
    if os.path.exists(os.path.join(p, "pyportal.toml"))
)
LIB = os.path.join(T, "airlift_lib")
try:
    fetch_libs(
        LIB, ["adafruit-circuitpython-esp32spi", "adafruit-circuitpython-connectionmanager"]
    )
except Exception as e:
    print("SKIP the AirLift's library cannot be fetched:", e)
    server.shutdown()
    sink.shutdown()
    sys.exit(0)
FILES = {}
for root, _, names in os.walk(LIB):
    for n in names:
        p = os.path.join(root, n)
        FILES["lib/" + os.path.relpath(p, LIB).replace(os.sep, "/")] = open(p, "rb").read()

AIRLIFT = (
    COMMON
    + """import board
from digitalio import DigitalInOut
from adafruit_esp32spi import adafruit_esp32spi
import adafruit_connection_manager

esp = adafruit_esp32spi.ESP_SPIcontrol(
    board.SPI(), DigitalInOut(board.ESP_CS), DigitalInOut(board.ESP_BUSY), DigitalInOut(board.ESP_RESET)
)
pool = adafruit_connection_manager.get_radio_socketpool(esp)

lab.panel("fault wifi.connect noap ssid=Gone once", "fault wifi.connect auth ssid=Lab once")
print("ap", attempt(lambda: esp.connect_AP("Gone", "secret12", timeout_s=1)), "|",
    attempt(lambda: esp.connect_AP("Lab", "secret12", timeout_s=1)), "|", attempt(lambda: esp.connect_AP("Lab", "secret12")))

lab.panel("fault net.dns fail host=localhost once")
print("dns", attempt(lambda: esp.get_host_by_name("localhost")), attempt(lambda: len(esp.get_host_by_name("localhost"))))

lab.panel("fault net.socket full once")
print("socket", attempt(esp.get_socket))

lab.panel("fault net.connect refuse port=%d once" % PORT)
print("connect", attempt(opened), attempt(opened))

lab.panel("fault net.send cut 7 once")
s = conn(5, SINK)
print("send cut", attempt(lambda: s.send(b"airlift data")))
s.close()

lab.panel("fault net.recv cut 3 once")
s = conn(5)
n = s.recv_into(buf)
print("recv cut", bytes(buf[:n]))
s.close()

s = conn(5)
lab.panel("fault wifi.link drop once")
print("link", esp.status, esp.is_connected)
s.close()
esp.connect_AP("Lab", "secret12")
print("relinked", esp.status, attempt(opened))
print("fired", lab.fired())
"""
)
with launch(
    AIRLIFT, open(os.path.join(BOARDS, "pyportal.toml")).read(), FILES, fast_time=True
) as b:
    out = b.wait("fired", timeout=90) + b.wait("\n", timeout=5)

check(
    "AirLift wifi.connect noap / auth: the library's ConnectionErrors, then it connects",
    "No such ssid" in line("ap")
    and "Failed to connect to ssid" in line("ap")
    and line("ap").endswith("| 3"),
    out,
)
check(
    "AirLift net.dns fail: the lookup fails once",
    line("dns") == "dns ConnectionError: Failed to request hostname 4",
    out,
)
check("AirLift net.socket full: no socket (ENFILE)", line("socket") == "socket 23", out)
check(
    "AirLift net.connect refuse: once, as a closed port fails there",
    line("connect") == "connect BrokenPipeError: Expected 01 but got 00 ok",
    out,
)
check(
    "AirLift net.send cut 7: the peer gets seven bytes",
    line("send cut").startswith("send cut ") and b"airlift" in GOT,
    (out, GOT),
)
check("AirLift net.recv cut 3: three bytes", line("recv cut") == "recv cut b'012'", out)
check("AirLift wifi.link drop: the connection lost (5)", line("link") == "link 5 False", out)
check("AirLift connecting again brings the link back", line("relinked") == "relinked 3 ok", out)

server.shutdown()
sink.shutdown()
