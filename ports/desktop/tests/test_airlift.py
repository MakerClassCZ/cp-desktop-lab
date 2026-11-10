"""AirLift: python3 test_airlift.py /path/to/circuitpython

An ESP32 with nina-fw on the virtual SPI bus (the board file's "airlift" line), driven by the
bundle's adafruit_esp32spi as on a PyPortal: its commands, and the network behind it, against
servers this test runs on 127.0.0.1. HTTPS trusts the test CA (airlift_ca.pem) through
CIRCUITPY_AIRLIFT_CAFILE; one public fetch SKIPs without a network.
"""

import http.server, os, re, socket, socketserver, ssl, sys, threading
from harness import HEADLESS, T, check, fetch_libs, launch, refused, seen

BOARDS = next(
    p
    for p in (os.path.join(T, "board_files"), os.path.join(T, "..", "boards", "files"))
    if os.path.exists(os.path.join(p, "pyportal.toml"))
)
PYPORTAL = open(os.path.join(BOARDS, "pyportal.toml")).read()

LIB = os.path.join(T, "airlift_lib")
try:
    fetch_libs(
        LIB,
        [
            "adafruit-circuitpython-esp32spi",
            "adafruit-circuitpython-requests",
            "adafruit-circuitpython-connectionmanager",
            "adafruit-circuitpython-busdevice",
        ],
    )
except Exception as e:
    print("SKIP the bundle's libraries cannot be fetched:", e)
    sys.exit(0)
FILES = {}
for root, _, names in os.walk(LIB):
    for n in names:
        p = os.path.join(root, n)
        FILES["lib/" + os.path.relpath(p, LIB).replace(os.sep, "/")] = open(p, "rb").read()

BODY = bytes(range(256)) * 80


class Pages(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = BODY if self.path == "/big" else b"hello"
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        data = self.request.recv(100)
        self.request.sendall(data)


class UdpEcho(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        sock.sendto(data, self.client_address)


def serve(server):
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


HTTP = serve(http.server.ThreadingHTTPServer(("127.0.0.1", 0), Pages))
ECHO = serve(socketserver.ThreadingTCPServer(("127.0.0.1", 0), Echo))
UDP = serve(socketserver.ThreadingUDPServer(("127.0.0.1", 0), UdpEcho))
tls_server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Pages)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(
    os.path.join(T, "airlift_server.pem"), os.path.join(T, "airlift_server.key")
)
tls_server.socket = context.wrap_socket(tls_server.socket, server_side=True)
HTTPS = serve(tls_server)
CLOSED = free_port()
SERVER = free_port()
TRACE = os.path.join(T, "airlift_trace.txt")
TRACED = ["--trace-bus", TRACE]


def traced():
    """The bus trace so far, all of it (the API keeps only its end)."""
    return open(TRACE, encoding="utf8", errors="replace").read()


START = """import board, time, wifi
from digitalio import DigitalInOut
from adafruit_esp32spi import adafruit_esp32spi
import adafruit_connection_manager, adafruit_requests

esp = adafruit_esp32spi.ESP_SPIcontrol(
    board.SPI(), DigitalInOut(board.ESP_CS), DigitalInOut(board.ESP_BUSY), DigitalInOut(board.ESP_RESET)
)
pool = adafruit_connection_manager.get_radio_socketpool(esp)
requests = adafruit_requests.Session(pool, adafruit_connection_manager.get_radio_ssl_context(esp))
BODY = bytes(range(256)) * 80
"""

PROGRAM = (
    START
    + """
print("fw", esp.firmware_version)
print("status", esp.status)
esp.connect_AP("Lab", "secret12")
print("status", esp.status)
wifi.radio.connect("Lab", "secret12")
print("ip same", esp.ipv4_address == str(wifi.radio.ipv4_address), esp.ipv4_address)
print("mac", len(esp.mac_address))
esp.start_scan_networks()
nets = esp.get_scan_networks()
print("scan", len(nets) > 0 and nets[0].ssid == esp.ap_info.ssid, [n.ssid for n in nets])
print("time", esp.get_time()[0] > 1700000000)
esp.set_pin_mode(25, 1)
esp.set_analog_write(25, 0.5)
for _ in range(2):
    try:
        esp._send_command_get_response(0x5F)
    except OSError as e:
        print("unknown", type(e).__name__)
print("dns", esp.pretty_ip(esp.get_host_by_name("localhost")))
try:
    esp.get_host_by_name("no.such.host.invalid")
    print("bad name resolved")
except ConnectionError:
    print("bad name refused")
r = requests.get("http://127.0.0.1:%(http)d/big")
body = r.content
print("http", r.status_code, len(body), body == BODY)
r.close()
s = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
s.settimeout(5)
s.connect(("127.0.0.1", %(echo)d))
s.send(b"ping")
buf = bytearray(4)
n = s.recv_into(buf)
print("echo", bytes(buf[:n]))
s.close()
s = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
try:
    s.connect(("127.0.0.1", %(closed)d))
    print("closed port connected")
except OSError as e:
    print("closed port refused", type(e).__name__)
s.close()
u = pool.socket(pool.AF_INET, pool.SOCK_DGRAM)
u.settimeout(5)
u.sendto(b"hello", ("127.0.0.1", %(udp)d))
buf = bytearray(16)
n = u.recv_into(buf)
print("udp", bytes(buf[:n]))
u.close()
srv = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
srv.bind(("0.0.0.0", %(server)d))
srv.listen(1)
print("listening")
while True:
    try:
        c, addr = srv.accept()
        break
    except OSError:
        time.sleep(0.05)
c.settimeout(5)
buf = bytearray(3)
n = c.recv_into(buf)
print("got", bytes(buf[:n]), "from", addr[0])
c.send(bytes(buf[:n]))
c.close()
srv.close()
s = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
s.connect(("127.0.0.1", %(echo)d))
print("before reset", esp.socket_status(s._socknum))
esp.reset()
print("after reset", esp.socket_status(s._socknum), esp.status)
print("done")
"""
    % dict(http=HTTP, echo=ECHO, closed=CLOSED, udp=UDP, server=SERVER)
)

with launch(PROGRAM, PYPORTAL, FILES, args=TRACED) as b:
    ok = seen(b, "listening", 30)
    out = b.console(since=0)["data"]
    check("firmware version 3.3.0", "fw 3.3.0" in out, out[-600:])
    check(
        "not connected (6), then connected (3)",
        re.search(r"status 6\r?\nstatus 3", out) is not None,
        out[-600:],
    )
    check("its address is the computer's", "ip same True" in out, out[-600:])
    check("a MAC of six bytes", "mac 6" in out, out[-600:])
    check("the scan finds the network it is on", "scan True" in out, out[-600:])
    check("the time is the computer's", "time True" in out, out[-600:])
    check(
        "an unknown command is refused (BrokenPipeError)",
        out.count("unknown BrokenPipeError") == 2,
        out[-600:],
    )
    said = "".join(b.stderr)
    check(
        "and said once on stderr",
        said.count("nina-fw command 0x5f is not modelled") == 1,
        said[-400:],
    )
    check("DNS: localhost", "dns 127.0.0.1" in out, out[-600:])
    check("DNS: a name that is not there fails", "bad name refused" in out, out[-600:])
    check(
        "HTTP through adafruit_requests: 20480 bytes, all of them",
        "http 200 20480 True" in out,
        out[-600:],
    )
    check("TCP echo", "echo b'ping'" in out, out[-600:])
    check(
        "a closed port fails as nina-fw does (an OSError)",
        "closed port refused BrokenPipeError" in out,
        out[-600:],
    )
    check("UDP echo", "udp b'hello'" in out, out[-600:])
    if ok:
        c = socket.create_connection(("127.0.0.1", SERVER), timeout=10)
        c.sendall(b"abc")
        back = c.recv(10)
        c.close()
    else:
        back = None
    check(
        "a server: the program accepts the computer's connection",
        seen(b, "got b'abc' from 127.0.0.1", 15),
        b.console(since=0)["data"][-400:],
    )
    check("a server: and answers", back == b"abc", back)
    seen(b, "done", 15)
    out = b.console(since=0)["data"]
    check(
        "a reset closes its sockets and forgets the network",
        "before reset 4" in out and "after reset 0 6" in out,
        out[-400:],
    )
    trace = traced()
    for name, pattern in (
        ("connect", r"AIRLIFT connect Lab"),
        ("the ESP32's own pins", r"AIRLIFT pin 25 mode 1.*AIRLIFT pin 25 pwm 127"),
        ("a socket to the HTTP server", r"AIRLIFT socket \d TCP 127\.0\.0\.1:%d" % HTTP),
        ("UDP", r"AIRLIFT socket \d UDP 127\.0\.0\.1:%d" % UDP),
        (
            "a server",
            r"AIRLIFT socket \d listen %d.*AIRLIFT socket \d from 127\.0\.0\.1:" % SERVER,
        ),
        ("closed", r"AIRLIFT socket \d closed"),
        ("reset", r"AIRLIFT reset"),
    ):
        check("trace: " + name, re.search(pattern, trace, re.S) is not None, trace[-600:])

TLS = (
    START
    + """
esp.connect_AP("Lab", "secret12")
try:
    r = requests.get("https://localhost:%(https)d/big")
    print("https", r.status_code, r.content == BODY)
    r.close()
except Exception as e:
    print("https failed", type(e).__name__, e)
print("done")
"""
    % dict(https=HTTPS)
)
ca = dict(HEADLESS, CIRCUITPY_AIRLIFT_CAFILE=os.path.join(T, "airlift_ca.pem"))
with launch(TLS, PYPORTAL, FILES, env=ca, args=TRACED) as b:
    seen(b, "done", 40)
    out = b.console(since=0)["data"]
    check("HTTPS to a server the CA signed: all of it", "https 200 True" in out, out[-600:])
    trace = traced()
    check(
        "trace: TLS",
        re.search(r"AIRLIFT socket \d TLS localhost:%d" % HTTPS, trace) is not None,
        trace[-400:],
    )
# TLS again after a soft reload: its memory (and the root bundle's) went with the VM heap.
FETCH = (
    START
    + """
import supervisor
esp.connect_AP("Lab", "secret12")
r = requests.get("https://localhost:%(https)d/big")
print("%%s 200 True" %% __file__ if r.status_code == 200 and r.content == BODY else r.status_code)
r.close()
"""
)
RELOAD = dict(FILES)
RELOAD["second.py"] = FETCH % dict(https=HTTPS) + 'print("second over")\n'
FIRST = (
    FETCH % dict(https=HTTPS) + 'supervisor.set_next_code_file("second.py")\nsupervisor.reload()\n'
)
with launch(FIRST, PYPORTAL, RELOAD, env=ca) as b:
    seen(b, "second over", 60)
    out = b.console(since=0)["data"]
    check(
        "HTTPS before and after a soft reload",
        "code.py 200 True" in out and "second.py 200 True" in out,
        out[-600:],
    )
with launch(TLS, PYPORTAL, FILES, args=TRACED) as b:
    seen(b, "done", 40)
    out = b.console(since=0)["data"]
    check(
        "the same server is not trusted with the bundle alone", "https failed" in out, out[-600:]
    )
    trace = traced()
    check("trace: why", "certificate not trusted" in trace, trace[-400:])

try:
    socket.getaddrinfo("www.adafruit.com", 443)
    online = True
except OSError:
    online = False
if online:
    PUBLIC = (
        START
        + """
esp.connect_AP("Lab", "secret12")
r = requests.get("https://www.adafruit.com/api/quotes.php")
print("public", r.status_code, r.json()[0]["author"] != "")
print("done")
"""
    )
    with launch(PUBLIC, PYPORTAL, FILES) as b:
        seen(b, "done", 40)
        out = b.console(since=0)["data"]
        check("HTTPS to adafruit.com through the bundle", "public 200 True" in out, out[-600:])
else:
    print("SKIP no network: the public HTTPS fetch")

# Another board: a Feather with the FeatherWing's pins (an add-on board file).
FEATHER = open(os.path.join(BOARDS, "feather.toml")).read()
WING = os.path.join(BOARDS, "airlift_featherwing.toml")
if os.path.exists(WING):
    with launch(
        START + 'print("fw", esp.firmware_version)\n', FEATHER, FILES, args=["--board-file", WING]
    ) as b:
        check(
            "a Feather with an AirLift FeatherWing",
            seen(b, "fw 3.3.0", 20),
            b.console(since=0)["data"][-400:],
        )

status, said = refused(board='[spi]\nESP = "airlift cs=GPIO1"\n')
check("an AirLift needs its ready and reset pins", "busy=" in said, said)
