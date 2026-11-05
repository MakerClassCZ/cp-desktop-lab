"""Bluetooth LE: python3 test_ble.py /path/to/circuitpython

_bleio over the HCI host stack: the adapter the port gives every run, --ble, and the virtual
controller with the phone the control API drives.
"""

import os, re, struct, time
from harness import HEADLESS, T, check, cpapi, launch, refused, run, seen

ADAPTER = """import _bleio
a = _bleio.adapter
print("adapter", a is not None, a and a.enabled)
if a:
    a.enabled = True
    print("enabled", a.enabled, len(str(a.address)) > 0)
"""

# The adapter is there before code.py, not yet enabled; enabling it runs the HCI init.
out, _ = run(ADAPTER, "", "enabled")
check("_bleio.adapter is there", "adapter True False" in out, out)
check("BLERadio enables it", "enabled True True" in out, out)

# A name set before anything enabled the adapter (adafruit_ble: BLERadio(), then ble.name = ...)
out, _ = run(
    'import _bleio\n_bleio.adapter.name = "CP-Lab"\nprint("name", _bleio.adapter.name)\n',
    "",
    "name",
)
check("the name set on an adapter not yet on", "name CP-Lab" in out, out[-300:])

with launch(ADAPTER, args=["--ble", "off"]) as b:
    seen(b, "adapter")
    seen(b, "\n")
    out = b.console(since=0)["data"]
    start = time.time()
    try:
        b.ble.connect()
        answer = None
    except cpapi.ApiError as e:
        answer = e.status
    took = time.time() - start
check("--ble off: no adapter", "adapter False" in out, out)
check("--ble off: the phone is refused at once", answer == 409 and took < 2, (answer, took))

out, _ = run(ADAPTER, "", "adapter", board='[board]\nble = "off"\n')
check("[board] ble = off", "adapter False" in out, out)

status, out = refused(["--ble", "NOPORT"])
check("a bad --ble port stops the start", status == 2 and "cannot open NOPORT" in out, out)

status, out = refused(["--fast-time", "--ble", "NOPORT"])
check(
    "--fast-time refuses --ble PORT", status == 2 and "real hardware needs real time" in out, out
)

# The phone, through the control API's raw endpoints: what the program advertises, a connection,
# ATT PDUs both ways (hex); and what the port sees of the HCI: the panel's row, the trace, btsnoop.
PERIPHERAL = """
import _bleio, time
a = _bleio.adapter
a.enabled = True
a.start_advertising(bytes([2, 1, 6, 7, 9]) + b"CP-lab", connectable=True)
print("advertising", "%02x" % a.address.address_bytes[5], a.address.type)
was = False
while True:
    if a.connected != was:
        was = a.connected
        print("connected" if was else "disconnected", a.connections)
    time.sleep(0.02)
"""


def att(b, pdu):
    """The phone sends pdu (hex); the program's first answer, hex, or None."""
    b.request("POST", "/api/ble/att", pdu)
    pdus = b.request("GET", "/api/ble/att?wait=3000")["pdus"]
    return pdus[0] if pdus else None


log = os.path.join(T, "ble.btsnoop")
if os.path.exists(log):
    os.remove(log)
with launch(PERIPHERAL, args=["--ble-log", log]) as b:
    seen(b, "advertising")
    state = b.request("GET", "/api/ble")
    row = b.state().get("ble")
    b.request("POST", "/api/ble/connect")
    connected = seen(b, "connected (<Connection>,)")
    links = b.request("GET", "/api/ble").get("links")
    # The controller sends a vendor event at each connection, as the CYW43439 does: the MTU
    # exchange after it still answers.
    mtu = att(b, "02f700")
    found = att(b, "060100ffff00280118")
    # An L2CAP frame longer than the host's 255-byte buffer, in two fragments: dropped.
    b.request("POST", "/api/ble/att", "52" + "0300" + "41" * 297)
    again = att(b, "060100ffff00280118")
    b.request("POST", "/api/ble/disconnect")
    disconnected = seen(b, "disconnected ()")
    links_after = b.request("GET", "/api/ble").get("links")
    trace = b.trace(since=0)["data"]
    out = b.console(since=0)["data"]
check(
    "advertising seen",
    state["advertising"] and state["connectable"] and "020106070943502d6c6162" in state["data"],
    state,
)
check(
    "the virtual controller has a public address",
    state["public"] and state["address"] != "00:00:00:00:00:00",
    state,
)
check(
    "the panel row",
    row is not None and row["state"] == "advertising" and row["name"] == "CP-lab",
    row,
)
check("connect: adapter.connections holds a Connection", connected, out[-300:])
check(
    "/api/ble links: the phone's, the program its peripheral, not encrypted; none after",
    links
    == [{"handle": 0x40, "peer": "7c:10:c9:1c:37:89", "role": "peripheral", "encrypted": False}]
    and links_after == [],
    (links, links_after),
)
check("MTU exchange after the vendor event", (mtu or "").startswith("03"), mtu)
check("a FIND_TYPE for GATT finds it", (found or "").startswith("07"), found)
check("a 300-byte frame in fragments does not break the host", again == found, again)
check("disconnect seen by the program", disconnected, out[-300:])
check(
    "the trace: advertising, connect, ATT both ways, disconnect",
    'BLE ADV "CP-lab" connectable 02 01 06 07 09 43 50 2d 6c 61 62' in trace
    and re.search(r"BLE CONNECT ([0-9a-f]{2}:){5}[0-9a-f]{2}", trace)
    and "BLE ATT <- MTU_REQ f7 00" in trace
    and "BLE ATT -> MTU_RSP " in trace
    and "BLE DISCONNECT 0x13" in trace,
    trace[-800:],
)
data = open(log, "rb").read() if os.path.exists(log) else b""
pos = 16
while pos + 24 <= len(data):
    pos += 24 + struct.unpack(">I", data[pos + 4 : pos + 8])[0]
check(
    "--ble-log is btsnoop: its header, then whole records",
    data[:8] == b"btsnoop\0"
    and struct.unpack(">II", data[8:16]) == (1, 1002)
    and pos == len(data) > 16,
    (data[:16], pos, len(data)),
)

# A controller without a public address (Zephyr's on an nRF): the host takes a static random one.
with launch(PERIPHERAL, env=dict(HEADLESS, CIRCUITPY_BLE_VIRTUAL_FAULT="no-public-address")) as b:
    seen(b, "advertising")
    seen(b, "\n")
    out = b.console(since=0)["data"]
    state = b.request("GET", "/api/ble")
words = out.split("advertising ")[-1].split()
check(
    "no public address: a static random one",
    len(words) >= 2 and int(words[0], 16) >= 0xC0 and words[1] == "1" and not state["public"],
    (out[-200:], state),
)

# The phone in cpapi: scan, GATT discovery, read, write, notifications, the Nordic UART
NUS = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
SERVICES = """
import _bleio, time
a = _bleio.adapter
a.enabled = True
OPEN, NONE = _bleio.Attribute.OPEN, _bleio.Attribute.NO_ACCESS
C = _bleio.Characteristic
nus = _bleio.Service(_bleio.UUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e"))
rx = C.add_to_service(nus, _bleio.UUID("6e400002-b5a3-f393-e0a9-e50e24dcca9e"),
    properties=C.WRITE | C.WRITE_NO_RESPONSE, read_perm=NONE, write_perm=OPEN, max_length=64)
tx = C.add_to_service(nus, _bleio.UUID("6e400003-b5a3-f393-e0a9-e50e24dcca9e"),
    properties=C.NOTIFY, read_perm=OPEN, write_perm=NONE, max_length=64)
mine = _bleio.Service(_bleio.UUID(0x1234))
hello = C.add_to_service(mine, _bleio.UUID(0x1235), properties=C.READ | C.WRITE,
    read_perm=OPEN, write_perm=OPEN, max_length=32, initial_value=b"hello")
C.add_to_service(mine, _bleio.UUID(0x1236), properties=C.READ, read_perm=OPEN, write_perm=NONE,
    max_length=22, initial_value=bytes(range(22)))
buf = _bleio.CharacteristicBuffer(rx, buffer_size=512)
uuid = bytes(reversed(bytes.fromhex("6e400001b5a3f393e0a9e50e24dcca9e")))
a.start_advertising(bytes([2, 1, 6, 7, 9]) + b"CP-lab" + bytes([17, 7]) + uuid, connectable=True)
print("ready")
last = b"hello"
while True:
    # At most what a notification takes at ble_hci's MTU of 23: 20 bytes.
    n = min(buf.in_waiting, 20)
    if n:
        tx.value = buf.read(n).upper()
    if hello.value != last:
        last = hello.value
        print("written", last)
    time.sleep(0.01)
"""
with launch(SERVICES) as b:
    seen(b, "ready")
    ad = b.ble.scan()
    check(
        "scan sees the name and the UART service", ad.name == "CP-lab" and NUS in ad.services, ad
    )
    p = b.ble.connect()
    uuids = [s.uuid for s in p.services()]
    check(
        "services lists GAP, GATT and the program's",
        all(cpapi.uuid(u) in uuids for u in (0x1800, 0x1801, NUS, 0x1234)),
        uuids,
    )
    name = p.read(0x2A00)
    check("read the device name", name.startswith(b"CIRCUITPY") and len(name) == 13, name)
    check("read a value", p.read(0x1235) == b"hello", p.read(0x1235))
    # 22 bytes fill a Read Response at MTU 23 exactly: the server ends it with an error to the Read Blob.
    try:
        whole = p.read(0x1236)
    except cpapi.AttError as e:
        whole = e
    check("read a value as long as a Read Response takes", whole == bytes(range(22)), whole)
    try:
        p.read(NUS_RX)
        error = None
    except cpapi.AttError as e:
        error = e
    check(
        "a characteristic the program does not let be read: an ATT error that names it",
        error is not None and error.code == 0x02 and "read not permitted" in str(error),
        str(error),
    )
    check(
        "a UUID without dashes, as nRF Connect copies it",
        cpapi.uuid("6E400001B5A3F393E0A9E50E24DCCA9E") == NUS,
        cpapi.uuid("6E400001B5A3F393E0A9E50E24DCCA9E"),
    )
    p.write(0x1235, b"world")
    check(
        "write, then the program has it", seen(b, "b'world')"), b.console(since=0)["data"][-300:]
    )
    p.uart_write("hi")
    check("UART both ways", p.uart_read(timeout=3) == "HI", p.uart_read(timeout=0))
    for i in range(100):
        p.write(NUS_RX, b"%02d" % i, response=False)
    echoed = ""
    for _ in range(50):
        echoed += p.uart_read(timeout=0.2)
        if len(echoed) >= 200:
            break
    check(
        "a flood of 100 writes keeps its order",
        echoed == "".join("%02d" % i for i in range(100)),
        echoed,
    )
    p.disconnect()

# BLERadio() code from a CLUE, with adafruit_ble (and adafruit_bluefruit_connect) from CIRCUITPY_LIB
LIB = os.environ.get("CIRCUITPY_LIB")


def lib_files(*names):
    """The libraries' files for a drive's lib/, or None when CIRCUITPY_LIB lacks one."""
    if not LIB or not all(os.path.isdir(os.path.join(LIB, n)) for n in names):
        return None
    files = {}
    for name in names:
        for root, _, found in os.walk(os.path.join(LIB, name)):
            for n in found:
                path = os.path.join(root, n)
                files["lib/" + os.path.relpath(path, LIB).replace(os.sep, "/")] = open(
                    path, "rb"
                ).read()
    return files


ECHO_RADIO = """
from adafruit_ble import BLERadio
from adafruit_ble.advertising.standard import ProvideServicesAdvertisement
from adafruit_ble.services.nordic import UARTService
ble = BLERadio()
uart = UARTService()
ble.start_advertising(ProvideServicesAdvertisement(uart))
print("ready")
while True:
    if ble.connected and uart.in_waiting:
        uart.write(uart.read(uart.in_waiting).upper())
"""
libs = lib_files("adafruit_ble")
if libs is None:
    print("SKIP BLERadio: set CIRCUITPY_LIB to a lib with adafruit_ble")
else:
    with launch(ECHO_RADIO, files=libs) as b:
        seen(b, "ready", 15)
        ad = b.ble.scan()
        p = b.ble.connect()
        p.uart_write("clue")
        check(
            "BLERadio and UARTService run unchanged",
            ad is not None and NUS in ad.services and p.uart_read(timeout=3) == "CLUE",
            (ad, b.console(since=0)["data"][-400:]),
        )
    out, _ = run(
        'from adafruit_ble import BLERadio\ntry:\n    BLERadio()\nexcept RuntimeError as e:\n    print("refused", e)\n',
        "",
        "refused",
        files=libs,
        board='[board]\nble = "off"\n',
    )
    check(
        "BLERadio on a board without Bluetooth raises RuntimeError", "refused" in out, out[-300:]
    )

libs = lib_files("adafruit_ble", "adafruit_bluefruit_connect")
if libs is None:
    print(
        "SKIP Bluefruit color packet: set CIRCUITPY_LIB to a lib with adafruit_ble and adafruit_bluefruit_connect"
    )
else:
    BLUEFRUIT = """
from adafruit_ble import BLERadio
from adafruit_ble.advertising.standard import ProvideServicesAdvertisement
from adafruit_ble.services.nordic import UARTService
from adafruit_bluefruit_connect.packet import Packet
from adafruit_bluefruit_connect.color_packet import ColorPacket
ble = BLERadio()
uart = UARTService()
ble.start_advertising(ProvideServicesAdvertisement(uart))
print("ready")
while True:
    if ble.connected and uart.in_waiting:
        packet = Packet.from_stream(uart)
        if isinstance(packet, ColorPacket):
            print("color", packet.color)
"""
    with launch(BLUEFRUIT, files=libs) as b:
        seen(b, "ready", 15)
        p = b.ble.connect()
        p.uart_write(cpapi.bluefruit.color(255, 0, 0))
        check(
            "Bluefruit color packet",
            seen(b, "color (255, 0, 0)"),
            b.console(since=0)["data"][-400:],
        )

# Disabling reads the reset's reply: left unread, it waited out the response timeout, and the next
# enable took it for the answer to its own reset (a CYW43 then dropped the commands that followed)
CYCLE = """
import _bleio, time
a = _bleio.adapter
worst = 0
for i in range(5):
    a.enabled = True
    t = time.monotonic()
    a.enabled = False
    worst = max(worst, time.monotonic() - t)
a.enabled = True
print("cycled", a.enabled, "worst off %.3f" % worst)
"""
out, _ = run(CYCLE, "", "cycled")
m = re.search(r"cycled True worst off ([0-9.]+)", out)
check(
    "disabling the adapter does not wait out the reset", m and float(m.group(1)) < 0.5, out[-300:]
)

# A connection stops advertising in the controller; stop_advertising() then asks nothing (a CYW43
# answers Command Disallowed, an ESP32 nothing at all)
STOP = """
import _bleio
a = _bleio.adapter
a.start_advertising(bytes([2, 1, 6]), connectable=True)
print("ready")
while not a.connected:
    pass
try:
    a.stop_advertising()
    print("stopped", a.advertising)
except Exception as e:
    print("stop raised", repr(e))
while a.connected:
    pass
"""
with launch(STOP) as b:
    seen(b, "ready", 15)
    try:
        b.ble.connect()  # a program that raised has ended: then nothing answers the phone
    except (TimeoutError, cpapi.ApiError):
        pass
    check(
        "stop_advertising() after a connection",
        seen(b, "stopped False"),
        b.console(since=0)["data"][-300:],
    )

# Five runs of a program that makes a service: the adapter stays, the heap does not shrink
MEMORY = """
import _bleio, gc
a = _bleio.adapter
s = _bleio.Service(_bleio.UUID(0x1234))
_bleio.Characteristic.add_to_service(s, _bleio.UUID(0x1235), properties=_bleio.Characteristic.READ,
    read_perm=_bleio.Attribute.OPEN, write_perm=_bleio.Attribute.NO_ACCESS, max_length=200, initial_value=bytes(200))
a.start_advertising(bytes([2, 1, 6]), connectable=True)
gc.collect()
print("free", gc.mem_free())
"""
with launch(MEMORY) as b:
    free = []
    for run_number in range(5):
        if run_number:
            b.reload()
        b.wait("free", timeout=10)
        b.wait("\n", timeout=5)
        free.append(int(b.console(since=0)["data"].rsplit("free ", 1)[1].split()[0]))
check("five runs of a BLE program keep the heap", max(free) - min(free) < 1024, free)

# The phone writes while the program sleeps: background HCI keeps going (and with --fast-time)
SLEEPER = """
import _bleio, time
a = _bleio.adapter
C = _bleio.Characteristic
s = _bleio.Service(_bleio.UUID(0x1234))
c = C.add_to_service(s, _bleio.UUID(0x1235), properties=C.WRITE, read_perm=_bleio.Attribute.OPEN,
    write_perm=_bleio.Attribute.OPEN, max_length=20)
a.start_advertising(bytes([2, 1, 6]), connectable=True)
print("ready")
while True:
    time.sleep(1)
    if c.value:
        print("got", bytes(c.value))
        break
"""
for fast in (False, True):
    with launch(SLEEPER, fast_time=fast) as b:
        seen(b, "ready")
        p = b.ble.connect()
        p.write(0x1235, b"zzz")
        check(
            "a write while the program sleeps%s" % (" (fast time)" if fast else ""),
            seen(b, "got b'zzz'", 10),
            b.console(since=0)["data"][-300:],
        )

# The adapter's name in the run after a reload: no pointer into the previous run's heap.
NAME = """
import _bleio, supervisor
if supervisor.runtime.run_reason == supervisor.RunReason.STARTUP:
    _bleio.adapter.enabled = True
    print("first", _bleio.adapter.name)
else:
    print("second", _bleio.adapter.name)
"""
with launch(NAME) as b:
    seen(b, "first")
    b.reload()
    ok = seen(b, "second CIRCUITPY", 10)
    out = b.console(since=0)["data"][-300:]
check("the adapter's name in the run after a reload", ok, out)

# discover_remote_services() on the phone's link: the program as a GATT client of a central
DISCOVER = """
import _bleio
a = _bleio.adapter
a.start_advertising(bytes([2, 1, 6]), connectable=True)
print("ready")
while not a.connected:
    pass
try:
    print("discovered", a.connections[0].discover_remote_services())
except Exception as e:
    print("discover raised", repr(e))
"""
with launch(DISCOVER) as b:
    seen(b, "ready")
    b.ble.connect().serve(2)  # the phone answers the program's requests
    ok = seen(b, "discovered ()")
    out = b.console(since=0)["data"][-300:]
check("discover_remote_services() on the phone's connection", ok, out)

# ATT as a server: the MTU a phone asks for, opcodes it does not know, Read By Type's lengths
ATT = """
import _bleio, time
a = _bleio.adapter
a.enabled = True
a.name = "CP-lab with a long name"
s = _bleio.Service(_bleio.UUID(0x1234))
_bleio.Characteristic.add_to_service(s, _bleio.UUID(0x1235), properties=_bleio.Characteristic.READ,
    read_perm=_bleio.Attribute.OPEN, write_perm=_bleio.Attribute.NO_ACCESS, max_length=8,
    initial_value=b"abc")
a.start_advertising(bytes([2, 1, 6]), connectable=True)
print("ready")
while True:
    time.sleep(0.1)
"""
with launch(ATT) as b:
    seen(b, "ready")
    b.request("POST", "/api/ble/connect")
    # A client's MTU under 23 is not one LE allows: the link stays at 23, the answer is the
    # server's own (Core Vol 3 Part F 3.4.2.2)
    mtu = att(b, "021000")
    name = att(b, "0a0300")  # the device name's value: 23 bytes, 22 of them in a Read Response
    quiet = {}
    for pdu in (
        "17030000004142",  # a Prepare Write Response nobody asked for
        "19",  # an Execute Write Response
        "21030041",  # a Read Multiple Variable Response
        "5f0300",  # a command no one knows
        "d2030041" + "00" * 12,  # a Signed Write Command: no answer, signed or not
    ):
        b.request("POST", "/api/ble/att", pdu)
        quiet[pdu] = b.request("GET", "/api/ble/att?wait=300")["pdus"]
    unsupported = att(b, "2003000500")  # a Read Multiple Variable Request
    by_type = att(b, "080100ffff3512")  # Read By Type of the 0x1235 value
check("an MTU request under 23: the server's 23", mtu == "031700", mtu)
check(
    "... and a Read Response still carries 22 bytes",
    name is not None and bytes.fromhex(name)[1:] == b"CP-lab with a long name"[:22],
    name,
)
check(
    "responses and commands get no Error Response",
    all(v == [] for v in quiet.values()),
    quiet,
)
check("a request not supported: Error 0x06", unsupported == "0120000006", unsupported)
check(
    "Read By Type of a value: the entry's length is handle plus value",
    by_type is not None and by_type[:4] == "0905" and by_type.endswith("616263"),
    by_type,
)
check(
    "... and its handle the value's, not the declaration's",
    by_type is not None and by_type[4:8] == "0c00",
    by_type,
)

# anonymous=True advertises from a random address the host set: a non-resolvable private one
ANONYMOUS = """
import _bleio, time
a = _bleio.adapter
a.start_advertising(bytes([2, 1, 6]), anonymous=True, timeout=60)
print("anonymous")
time.sleep(1.5)
a.stop_advertising()
a.start_advertising(bytes([2, 1, 6]), connectable=True)
print("normal")
while True:
    time.sleep(0.1)
"""


def adv_address(b, text):
    seen(b, text)
    time.sleep(0.2)
    s = b.request("GET", "/api/ble")
    return s["public"], s["address"]


with launch(ANONYMOUS) as b:
    anonymous = adv_address(b, "anonymous")
    normal = adv_address(b, "normal")
with launch(ANONYMOUS, env=dict(HEADLESS, CIRCUITPY_BLE_VIRTUAL_FAULT="no-public-address")) as b:
    anonymous_static = adv_address(b, "anonymous")
    normal_static = adv_address(b, "normal")
check(
    "anonymous: a non-resolvable private address the host set",
    anonymous[0] is False
    and anonymous[1] != "00:00:00:00:00:00"
    and int(anonymous[1][:2], 16) < 0x40,
    anonymous,
)
check("... then the public one again", normal[0] is True, normal)
check(
    "anonymous without a public address, then the static one again",
    anonymous_static[0] is False
    and int(anonymous_static[1][:2], 16) < 0x40
    and normal_static[0] is False
    and int(normal_static[1][:2], 16) >= 0xC0,
    (anonymous_static, normal_static),
)

# Security modes: a link that is not encrypted gets what OPEN allows, nothing more
SECURE = """
import _bleio, time
a = _bleio.adapter
A, C = _bleio.Attribute, _bleio.Characteristic
s = _bleio.Service(_bleio.UUID(0x1240))
RW = C.READ | C.WRITE | C.WRITE_NO_RESPONSE
enc = C.add_to_service(s, _bleio.UUID(0x1241), properties=RW | C.NOTIFY,
    read_perm=A.ENCRYPT_NO_MITM, write_perm=A.ENCRYPT_NO_MITM, max_length=8, initial_value=b"secret")
mitm = C.add_to_service(s, _bleio.UUID(0x1242), properties=RW,
    read_perm=A.ENCRYPT_WITH_MITM, write_perm=A.ENCRYPT_WITH_MITM, max_length=8, initial_value=b"mitm")
lesc = C.add_to_service(s, _bleio.UUID(0x1243), properties=RW,
    read_perm=A.LESC_ENCRYPT_WITH_MITM, write_perm=A.LESC_ENCRYPT_WITH_MITM, max_length=8,
    initial_value=b"lesc")
signed = C.add_to_service(s, _bleio.UUID(0x1244), properties=RW,
    read_perm=A.SIGNED_NO_MITM, write_perm=A.SIGNED_NO_MITM, max_length=8, initial_value=b"signed")
hidden = C.add_to_service(s, _bleio.UUID(0x1245), properties=RW,
    read_perm=A.NO_ACCESS, write_perm=A.NO_ACCESS, max_length=8, initial_value=b"hidden")
tick = C.add_to_service(s, _bleio.UUID(0x1246), properties=RW | C.NOTIFY,
    read_perm=A.OPEN, write_perm=A.OPEN, max_length=8, initial_value=b"open")
_bleio.Descriptor.add_to_characteristic(tick, _bleio.UUID(0x2901), read_perm=A.ENCRYPT_NO_MITM,
    write_perm=A.NO_ACCESS, max_length=8, initial_value=b"desc")
a.start_advertising(bytes([2, 1, 6]), connectable=True)
print("ready")
values = [c.value for c in (enc, mitm, lesc, signed, hidden)]
while True:
    now = [c.value for c in (enc, mitm, lesc, signed, hidden)]
    if now != values:
        values = now
        print("changed", values)
    if tick.value == b"go":
        # Notifications on for both, as a bonded phone's restored CCCDs would be: only the open
        # one may go out on a link that is not encrypted.
        for c in (enc, tick):
            c.descriptors[0].value = b"\\x01\\x00"
        enc.value = b"leak"
        tick.value = b"tick"
        values = [c.value for c in (enc, mitm, lesc, signed, hidden)]
        print("notified")
    time.sleep(0.02)
"""


def att_error(f):
    """The ATT error code f raises, or what it returned."""
    try:
        return ("returned", f())
    except cpapi.AttError as e:
        return e.code
    except TimeoutError as e:
        return ("timeout", str(e))


with launch(SECURE) as b:
    seen(b, "ready")
    p = b.ble.connect(auto_pair=False)  # the bare refusals, not a phone that pairs on them
    found = [c.uuid for s in p.services() for c in s.characteristics]
    reads = {u: att_error(lambda u=u: p.read(u)) for u in (0x1241, 0x1242, 0x1243, 0x1244, 0x1245)}
    writes = {
        u: att_error(lambda u=u: p.write(u, b"x"))
        for u in (0x1241, 0x1242, 0x1243, 0x1244, 0x1245)
    }
    for u in (0x1241, 0x1245):
        p.write(u, b"x", response=False)
    enc = p.characteristic(0x1241)
    subscribe = att_error(lambda: p.subscribe(0x1241))
    blob = att_error(lambda: p._request(b"\x0c" + struct.pack("<HH", enc.handle, 0), 0x0D))
    by_type = att_error(lambda: p._request(b"\x08" + struct.pack("<HHH", 1, 0xFFFF, 0x1241), 0x09))
    prepare = att_error(
        lambda: p._request(b"\x16" + struct.pack("<HH", enc.handle, 0) + b"x", 0x17)
    )
    tick = p.characteristic(0x1246)
    desc = att_error(lambda: p._request(b"\x0a" + struct.pack("<H", tick.handle + 2), 0x0B))
    open_read = att_error(lambda: p.read(0x1246))
    p.write(0x1246, b"go")
    notified = seen(b, "notified")
    p._take(1)
    got = {h: v for h, v in p._notified.items()}
    out = b.console(since=0)["data"]
check(
    "discovery is open on a link that is not encrypted",
    all(cpapi.uuid(u) in found for u in range(0x1241, 0x1247)),
    found,
)
check(
    "reads: encryption needed 0x05, NO_ACCESS 0x02",
    reads == {0x1241: 0x05, 0x1242: 0x05, 0x1243: 0x05, 0x1244: 0x05, 0x1245: 0x02},
    reads,
)
check(
    "writes: encryption needed 0x05, NO_ACCESS 0x03",
    writes == {0x1241: 0x05, 0x1242: 0x05, 0x1243: 0x05, 0x1244: 0x05, 0x1245: 0x03},
    writes,
)
check("Write Commands change nothing", "changed" not in out, out[-300:])
check("the CCCD of an encrypted characteristic: 0x05", subscribe == 0x05, subscribe)
check("Read Blob of it: 0x05", blob == 0x05, blob)
check("Read By Type of its value: 0x05", by_type == 0x05, by_type)
check("Prepare Write to it: 0x05", prepare == 0x05, prepare)
check("a descriptor that needs encryption: 0x05", desc == 0x05, desc)
check("an OPEN characteristic reads as before", open_read == ("returned", b"open"), open_read)
check(
    "notifications only of what the link may read",
    notified and got.get(tick.handle) == [b"tick"] and enc.handle not in got,
    (notified, got),
)

# erase_bonding() with no bonds, before and after the adapter is on
ERASE = """
import _bleio
a = _bleio.adapter
for step in ("off", "on"):
    if step == "on":
        a.enabled = True
    try:
        a.erase_bonding()
        print("erased", step, a.enabled)
    except Exception as e:
        print("erase raised", step, repr(e))
print("end")
"""
out, _ = run(ERASE, "", "end")
check(
    "erase_bonding() with the adapter off and on",
    "erased off False" in out and "erased on True" in out,
    out[-300:],
)

print("DONE")
