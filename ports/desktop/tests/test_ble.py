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
    # The controller sends a vendor event at each connection, as the CYW43439 does: the MTU
    # exchange after it still answers.
    mtu = att(b, "02f700")
    found = att(b, "060100ffff00280118")
    # An L2CAP frame longer than the host's 255-byte buffer, in two fragments: dropped.
    b.request("POST", "/api/ble/att", "52" + "0300" + "41" * 297)
    again = att(b, "060100ffff00280118")
    b.request("POST", "/api/ble/disconnect")
    disconnected = seen(b, "disconnected ()")
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
        error = e.code
    check(
        "a characteristic the program does not let be read: an ATT error", error is not None, error
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

# A controller that never gives the host its ACL credit back: the program goes on, its sends fail.
HEARTBEAT = """
import _bleio, time
a = _bleio.adapter
a.start_advertising(bytes([2, 1, 6]), connectable=True)
print("ready")
n = 0
while True:
    n += 1
    print("beat", n)
    time.sleep(0.1)
"""
with launch(HEARTBEAT, env=dict(HEADLESS, CIRCUITPY_BLE_VIRTUAL_FAULT="no-completed")) as b:
    seen(b, "ready")
    b.request("POST", "/api/ble/connect")
    for _ in range(12):
        b.request("POST", "/api/ble/att", "02f700")
    before = b.console(since=0)["data"].count("beat")
    goes_on = seen(b, "beat %d\r\n" % (before + 10), 10)
check("a controller that keeps the ACL credit does not hang the program", goes_on, before)

# A controller that does not answer: the first use fails, and the adapter is not left half on.
SILENT = """
import _bleio
a = _bleio.adapter
try:
    a.enabled = True
except Exception as e:
    print("failed", type(e).__name__)
print("enabled", a.enabled)
"""
with launch(SILENT, env=dict(HEADLESS, CIRCUITPY_BLE_VIRTUAL_FAULT="silent")) as b:
    seen(b, "enabled", 20)
    out = b.console(since=0)["data"][-300:]
check(
    "a controller that does not answer leaves the adapter off",
    "failed" in out and "enabled False" in out,
    out,
)

print("DONE")
