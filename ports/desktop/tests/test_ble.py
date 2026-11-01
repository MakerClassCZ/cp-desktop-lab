"""Bluetooth LE: python3 test_ble.py /path/to/circuitpython

_bleio over the HCI host stack: the adapter the port gives every run, --ble, and the virtual
controller with the phone the control API drives.
"""

from harness import check, cpapi, launch, refused, run, seen

ADAPTER = 'import _bleio\nprint("adapter", _bleio.adapter is not None, _bleio.adapter and _bleio.adapter.enabled)\n'

# The adapter is there before code.py, not yet enabled
out, _ = run(ADAPTER, "", "adapter")
check("_bleio.adapter is there", "adapter True False" in out, out)

# Enabling it runs the HCI init with the virtual controller
out, _ = run(
    "import _bleio\n_bleio.adapter.enabled = True\n"
    'print("enabled", _bleio.adapter.enabled, len(str(_bleio.adapter.address)) > 0)\n',
    "",
    "enabled",
)
check("BLERadio enables it", "enabled True True" in out, out)

out, _ = run(ADAPTER, "", "adapter", args=["--ble", "off"])
check("--ble off: no adapter", "adapter False" in out, out)

out, _ = run(ADAPTER, "", "adapter", board='[board]\nble = "off"\n')
check("[board] ble = off", "adapter False" in out, out)

status, out = refused(["--ble", "NOPORT"])
check("a bad --ble port stops the start", status == 2 and "cannot open NOPORT" in out, out)

status, out = refused(["--fast-time", "--ble", "NOPORT"])
check(
    "--fast-time refuses --ble PORT", status == 2 and "real hardware needs real time" in out, out
)

# A reload frees what code.py made, but the port's adapter stays bound
with launch('import _bleio\n_bleio.adapter.enabled = True\nprint("enabled")\n') as b:
    seen(b, "enabled")
    b.reload()
    seen(b, "enabled")
    out = b.exec('import _bleio\nprint("bound", _bleio.adapter is not None)', interrupt=True)
check("a reload keeps the adapter", "bound True" in out, out)

# The phone, through the control API's raw endpoints: what the program advertises, a connection,
# ATT PDUs both ways (hex).
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
        print("connected" if was else "disconnected", len(a.connections))
    time.sleep(0.02)
"""


def att(b, pdu):
    """The phone sends pdu (hex); the program's first answer, hex, or None."""
    b.request("POST", "/api/ble/att", pdu)
    pdus = b.request("GET", "/api/ble/att?wait=3000")["pdus"]
    return pdus[0] if pdus else None


with launch(PERIPHERAL) as b:
    seen(b, "advertising")
    state = b.request("GET", "/api/ble")
    check(
        "advertising seen",
        state["advertising"]
        and state["connectable"]
        and "020106070943502d6c6162" in state["data"],
        state,
    )
    b.request("POST", "/api/ble/connect")
    check("connect", seen(b, "connected 1"), b.console(since=0)["data"])
    # The controller sends a vendor event at each connection, as the CYW43439 does.
    mtu = att(b, "02f700")
    check("an unknown vendor event does not stop the host", mtu is not None, mtu)
    check("MTU exchange", (mtu or "").startswith("03"), mtu)
    found = att(b, "060100ffff00280118")
    check("a FIND_TYPE for GATT finds it", (found or "").startswith("07"), found)
    groups = att(b, "100100ffff0028")
    check(
        "read group (services)",
        (groups or "").startswith("11") and "0018" in groups and "0118" in groups,
        groups,
    )
    b.request("POST", "/api/ble/disconnect")
    check("disconnect seen by the program", seen(b, "disconnected 0"), b.console(since=0)["data"])

# A controller without a public address (Zephyr's on an nRF): the host takes a static random one.
from harness import HEADLESS

with launch(PERIPHERAL, env=dict(HEADLESS, CIRCUITPY_BLE_NO_PUBLIC_ADDRESS="1")) as b:
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

# What the port sees of the HCI both ways: the panel's row, the trace, a btsnoop file
import os, struct
from harness import T

log = os.path.join(T, "ble.btsnoop")
if os.path.exists(log):
    os.remove(log)
with launch(PERIPHERAL, args=["--ble-log", log]) as b:
    seen(b, "advertising")
    row = b.state().get("ble")
    b.request("POST", "/api/ble/connect")
    seen(b, "connected 1")
    att(b, "02f700")
    b.request("POST", "/api/ble/disconnect")
    seen(b, "disconnected 0")
    trace = b.trace(since=0)["data"]
check(
    "the panel row",
    row is not None and row["state"] == "advertising" and row["name"] == "CP-lab",
    row,
)
for line in (
    'BLE ADV "CP-lab" connectable 02 01 06 07 09 43 50 2d 6c 61 62',
    "BLE CONNECT 7c:10:c9:1c:37:89",
    "BLE ATT <- MTU_REQ f7 00",
    "BLE ATT -> MTU_RSP 17 00",
    "BLE DISCONNECT 0x13",
):
    check("the trace: " + line, line in trace, trace[-800:])

data = open(log, "rb").read() if os.path.exists(log) else b""
records, pos = [], 16
while pos + 24 <= len(data):
    orig, incl, flags, drops, stamp = struct.unpack(">IIIIq", data[pos : pos + 24])
    records.append((flags, data[pos + 24 : pos + 24 + incl]))
    pos += 24 + incl
check(
    "--ble-log is btsnoop",
    data[:8] == b"btsnoop\0"
    and struct.unpack(">II", data[8:16]) == (1, 1002)
    and pos == len(data)
    and len(records) >= 10
    # The phone's MTU request, received by the host: an ACL packet on the ATT channel.
    and any(f & 1 and p[:1] == b"\x02" and p[9:10] == b"\x02" for f, p in records),
    (data[:16], pos, len(data), len(records)),
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

# BLERadio and the Bluefruit Connect packets, with the libraries when there are some
LIB = os.environ.get("CIRCUITPY_LIB")
if (
    not LIB
    or not os.path.isdir(os.path.join(LIB, "adafruit_ble"))
    or not os.path.isdir(os.path.join(LIB, "adafruit_bluefruit_connect"))
):
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
    libs = {}
    for name in ("adafruit_ble", "adafruit_bluefruit_connect"):
        for root, _, names in os.walk(os.path.join(LIB, name)):
            for n in names:
                path = os.path.join(root, n)
                libs[os.path.relpath(path, LIB).replace(os.sep, "/").join(("lib/", ""))] = open(
                    path, "rb"
                ).read()
    with launch(BLUEFRUIT, files=libs) as b:
        seen(b, "ready", 15)
        p = b.ble.connect()
        p.uart_write(cpapi.bluefruit.color(255, 0, 0))
        check(
            "Bluefruit color packet",
            seen(b, "color (255, 0, 0)"),
            b.console(since=0)["data"][-400:],
        )

print("DONE")
