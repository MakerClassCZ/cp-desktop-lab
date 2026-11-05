"""Bluetooth LE, the program as central: python3 test_ble_central.py /path/to/circuitpython

The virtual controller's air: devices the test plays (cpapi.Peripheral, cpapi.devices) that the
program scans for, connects to and reads as a GATT client; and two programs on one air (CLUE to
CLUE) through cpapi.Air's relay.
"""

import os, threading, time
from harness import BIN, HEADLESS, check, cpapi, drive, lib_files, seen

# The REPL holds the program's variables from one exec to the next, so a test drives the program
# step by step while the Air's threads play the devices.
IDLE = "print('idle')\n"


def start(name="CIRCUITPY", code=IDLE, files=None, env=None, args=()):
    """A program on a drive of its own (name), at the REPL once code.py is done."""
    b = cpapi.Board.launch(
        BIN, drive(code, files, name), env=dict(HEADLESS, **(env or {})), args=args
    )
    b.wait("idle" if code == IDLE else "\n", timeout=15)
    return b


def ex(b, code, timeout=20):
    """code run at the program's REPL: what it printed, or the error's text."""
    try:
        return b.exec(code, interrupt=True, timeout=timeout)
    except cpapi.CircuitPythonError as e:
        return "ERROR %s %s" % (e, getattr(e, "output", ""))


SETUP = "import _bleio, time\na = _bleio.adapter\na.enabled = True\n"

# What a scan sees: each address once with its first entry of each kind.
SCAN = """
found = {}
for e in a.start_scan(%s):
    k = (e.address.address_bytes, e.scan_response)
    if k not in found:
        found[k] = e
        print("entry", ":".join("%%02x" %% x for x in reversed(e.address.address_bytes)),
            e.address.type, e.rssi, e.connectable, e.scan_response,
            bytes(e.advertisement_bytes).hex())
print("scanned", len(found))
"""


def entries(out):
    """The entries a SCAN printed: [(address, type, rssi, connectable, scan_response, data)]."""
    got = []
    for line in out.splitlines():
        if line.startswith("entry "):
            _, address, kind, rssi, connectable, sr, data = (line.split() + [""])[:7]
            got.append((address, int(kind), int(rssi), connectable == "True", sr == "True", data))
    return got


# ---- the address per drive ----

ADDRESS = SETUP + "print('address', a.address)\n"
with start("CIRCUITPY") as b1, start("CIRCUITPY2") as b2:
    a1, a2 = ex(b1, ADDRESS), ex(b2, ADDRESS)
    n1 = ex(b1, "print(a.name)")
with start("CIRCUITPY", env={"CIRCUITPY_BLE_ADDRESS": "02:dc:12:34:56:78"}) as b:
    a3 = ex(b, ADDRESS)
    n3 = ex(b, "print(a.name)")
check(
    "two drives, two addresses",
    "02:dc:" in a1.lower() and "02:dc:" in a2.lower() and a1 != a2,
    (a1, a2),
)
check(
    "CIRCUITPY_BLE_ADDRESS sets it, and the default name with it",
    "02:dc:12:34:56:78" in a3.lower() and n1 != n3,
    (a3, n1, n3),
)

# ---- scanning ----

IBEACON = bytes.fromhex("0201061aff4c000215" + "11" * 16 + "00010002c5")

with start() as b, cpapi.Air(b) as air:
    beacon = air.add(cpapi.devices.beacon(IBEACON, rssi=-60, address="c1:22:33:44:55:66"))
    ex(b, SETUP)
    out = ex(b, SCAN % "timeout=0.5")
    got = entries(out)
    state = b.request("GET", "/api/ble")
    trace = b.trace(since=0)["data"]
check(
    "[1] a beacon: one entry, RANDOM_STATIC, -60, not connectable, its bytes",
    got == [("c1:22:33:44:55:66", 1, -60, False, False, IBEACON.hex())],
    out,
)
check("[1] the trace: BLE SCAN on active", "BLE SCAN on active" in trace, trace[-600:])
check(
    "[1] the trace: one BLE REPORT per address and data",
    trace.count("BLE REPORT c1:22:33:44:55:66 ADV_NONCONN_IND -60") == 1,
    trace[-600:],
)
check("[4] start_scan(timeout=0.5) ends by itself", "BLE SCAN off" in trace, trace[-300:])
check("[1] /api/ble: scanning false after it", state.get("scanning") is False, state)

with start() as b, cpapi.Air(b) as air:
    hrm = air.add(cpapi.devices.heart_rate(scan_response=bytes.fromhex("04ff34120a")))
    weak = air.add(cpapi.devices.heart_rate(name="FAR", rssi=-90))
    ex(b, SETUP)
    active = entries(ex(b, SCAN % "timeout=0.5"))
    passive = entries(ex(b, SCAN % "timeout=0.5, active=False"))
    near = entries(ex(b, SCAN % "timeout=0.5, minimum_rssi=-100"))
    # A prefix is length-encoded: 3, then an AD structure's type and data (ff 34 12).
    prefixed = entries(ex(b, SCAN % "prefixes=b'\\x03\\xff\\x34\\x12', timeout=0.5"))
    beacon = air.add(cpapi.devices.beacon(IBEACON))
    endless = ex(
        b,
        "n = 0\nfor e in a.start_scan():\n    n += 1\n    if n == 3:\n        a.stop_scan()\n"
        "print('stopped', n)\n",
    )
    again = entries(ex(b, SCAN % "timeout=0.5"))
    trace = b.trace(since=0)["data"]
addresses = lambda got: sorted(set(g[0] for g in got))  # noqa: E731
check(
    "[2] active: the advertisement and the scan response",
    [g[4] for g in active if g[0] == hrm.address] == [False, True],
    active,
)
check(
    "[2] passive: no scan response",
    [g[4] for g in passive if g[0] == hrm.address] == [False],
    passive,
)
check(
    "[3] minimum_rssi: -90 not seen with the default -80, seen with -100",
    weak.address not in addresses(active) and weak.address in addresses(near),
    (active, near),
)
check("[3] prefixes filter", addresses(prefixed) == [hrm.address], prefixed)
check("[4] stop_scan() ends a scan with no timeout", "stopped 3" in endless, endless)
check(
    "[4] a scan after it sees the devices again",
    hrm.address in addresses(again) and beacon.address in addresses(again),
    again,
)
check(
    "[2] the trace names the report kinds",
    "ADV_IND" in trace and "SCAN_RSP" in trace and '"HRM"' in trace,
    trace[-800:],
)

# ---- connecting ----


def connect(address, timeout=3, random=True):
    """Code that connects the program to address (as c) and prints how it went."""
    kind = "RANDOM_STATIC" if random else "PUBLIC"
    return (
        "addr = _bleio.Address(bytes(reversed(bytes.fromhex(%r))), _bleio.Address.%s)\n"
        "t = time.monotonic()\n"
        "try:\n"
        "    c = a.connect(addr, timeout=%r)\n"
        "    print('connected', c.connected, len(a.connections), round(time.monotonic() - t, 1))\n"
        "except Exception as e:\n"
        "    print('failed', repr(e), round(time.monotonic() - t, 1))\n"
    ) % (address.replace(":", ""), kind, timeout)


with start() as b, cpapi.Air(b) as air:
    hrm = air.add(cpapi.devices.heart_rate(address="c5:11:22:33:44:55"))
    ex(b, SETUP)
    first = ex(b, connect(hrm.address))
    device_sees = hrm.wait_connected()
    interval = ex(b, "print('interval', c.connection_interval)")
    imported = ex(b, "import _bleio\nprint('still', c.connected, len(_bleio.adapter.connections))")
    links = b.request("GET", "/api/ble")["links"]
    trace = b.trace(since=0)["data"]
    hrm.disconnect()
    dropped = ex(b, "time.sleep(0.3)\nprint('after', c.connected, len(a.connections))")
    again = ex(b, connect(hrm.address))
    ex(b, "c.disconnect()\ntime.sleep(0.3)")
    device_dropped = hrm.wait_connected(False)
    nobody = ex(b, connect("c9:99:99:99:99:99", timeout=1))
    trace_cancel = b.trace(since=0)["data"]
    after_cancel = ex(b, connect(hrm.address))
check(
    "[6] connect: connected, in adapter.connections",
    first.startswith("connected True 1") and device_sees,
    (first, device_sees),
)
check(
    "[6] the trace: BLE CONNECT ... central",
    "BLE CONNECT c5:11:22:33:44:55 central" in trace,
    trace[-400:],
)
check(
    "[6] /api/ble links: the program central",
    [(x["peer"], x["role"]) for x in links] == [("c5:11:22:33:44:55", "central")],
    links,
)
check("[6] connection_interval from the connection: 7.5 ms", "interval 7.5" in interval, interval)
check("[6] _bleio imported again keeps the connection", "still True 1" in imported, imported)
check("[9] the device disconnects: the program sees it", "after False 0" in dropped, dropped)
check(
    "[9] the program disconnects: the device sees it",
    again.startswith("connected True") and device_dropped,
    (again, device_dropped),
)
took = float(nobody.split()[-1]) if nobody.startswith("failed") else None
check(
    "[7] nobody there, timeout=1: BluetoothError in 1 to 2 s",
    "BluetoothError" in nobody and "timeout" in nobody and took is not None and 0.9 <= took <= 2.5,
    nobody,
)
check("[7] Cancel in the trace", "BLE CONNECT cancel" in trace_cancel, trace_cancel[-400:])
check("[7] a connect afterwards works", after_cancel.startswith("connected True"), after_cancel)

with start() as b, cpapi.Air(b) as air:
    hrm = air.add(cpapi.devices.heart_rate())
    ex(b, SETUP)
    while_scanning = ex(
        b,
        "results = a.start_scan()\nfor e in results:\n    break\n"
        + connect(hrm.address)
        + "print('scanning after', a.scan_results_done if hasattr(a, 'scan_results_done') else '')\n",
    )
    state = b.request("GET", "/api/ble")
    trace = b.trace(since=0)["data"]
check(
    "[8] connect while scanning: the scan stops, the connect succeeds",
    "connected True" in while_scanning and state["scanning"] is False and "BLE SCAN off" in trace,
    (while_scanning, state),
)

# The phone connected to the program and the program connected to a sensor: two links.
PHONE_AND_SENSOR = (
    SETUP + "a.start_advertising(bytes([2, 1, 6, 4, 9]) + b'Lab', connectable=True)\n"
)
with start() as b, cpapi.Air(b) as air:
    hrm = air.add(cpapi.devices.heart_rate())
    ex(b, PHONE_AND_SENSOR)
    phone = b.ble.connect()
    both = ex(b, connect(hrm.address))
    links = b.request("GET", "/api/ble")["links"]
    name = phone.read(0x2A00)
check(
    "[10] the phone and a sensor: two links, both work",
    "connected True 2" in both
    and sorted(x["role"] for x in links) == ["central", "peripheral"]
    and len(set(x["handle"] for x in links)) == 2
    and name.startswith(b"CIRCUITPY"),
    (both, links, name),
)

# ---- the GATT client ----

DISCOVER = """
C = _bleio.Characteristic
services = c.discover_remote_services(%s)
for s in services:
    print("service", s.uuid, s.remote)
    for ch in s.characteristics:
        flags = [n for n in ("read", "write", "write_no_response", "notify", "indicate")
            if ch.properties & getattr(C, n.upper())]
        print("  char", ch.uuid, ",".join(flags), [str(d.uuid) for d in ch.descriptors])
print("discovered", len(services))
"""

LONG = bytes(range(256)) + bytes(range(44))  # 300 bytes: more than an MTU of 247 reads at once
with start() as b, cpapi.Air(b) as air:
    hrm = cpapi.devices.heart_rate(address="c6:00:00:00:00:01")
    hrm.MTU = 100  # a device's MTU smaller than the program's 247
    air.add(hrm)
    extra = cpapi.Peripheral(
        "Extra",
        [
            cpapi.GattService(
                0x1234,
                [
                    cpapi.GattCharacteristic(0x1235, ("read",), LONG),
                    cpapi.GattCharacteristic(0x1236, ("read",), b"no", error=0x02),
                    cpapi.GattCharacteristic(0x1237, ("write",)),
                    cpapi.GattCharacteristic(0x1238, ("write_no_response",)),
                    cpapi.GattCharacteristic(0x1239, ("read",), b"ro"),
                    cpapi.GattCharacteristic(0x123A, ("write",), error=0x03),
                    cpapi.GattCharacteristic(0x123B, ("indicate",)),
                    cpapi.GattCharacteristic(0x123C, ("notify",)),
                ],
            )
        ],
        address="c6:00:00:00:00:02",
    )
    air.add(extra)
    ex(b, SETUP)
    ex(b, connect(hrm.address))
    found = ex(b, DISCOVER % "")
    mtu = ex(b, "print('packet', c.max_packet_length)")
    trace = b.trace(since=0)["data"]
    b.trace_offset = b.trace(since=0)["end"]
    only = ex(b, DISCOVER % "[_bleio.UUID(0x180D)]")
    trace_only = b.follow("trace")[0]
    read = ex(
        b,
        "hr = [s for s in services if s.uuid == _bleio.UUID(0x180D)][0]\n"
        "loc = [ch for ch in hr.characteristics if ch.uuid == _bleio.UUID(0x2A38)][0]\n"
        "print('location', bytes(loc.value))\n",
    )
    ex(b, "c.disconnect()\ntime.sleep(0.2)")
    ex(b, connect(extra.address))
    ex(b, DISCOVER % "")
    ex(b, "ch = {x.uuid: x for s in services for x in s.characteristics}\nU = _bleio.UUID\n")
    long_read = ex(
        b,
        "print('long', ch[U(0x1235)].value == bytes(range(256)) + bytes(range(44)), "
        "len(ch[U(0x1235)].value))",
    )
    trace_long = b.follow("trace")[0]
    refused = ex(b, "ch[U(0x1236)].value")
    ex(b, "ch[U(0x1237)].value = b'request'\nch[U(0x1238)].value = b'command'\ntime.sleep(0.2)")
    request, command = extra.written(0x1237), extra.wait_written(0x1238)
    trace_writes = b.follow("trace")[0]
    read_only = ex(b, "ch[U(0x1239)].value = b'x'")
    write_refused = ex(b, "ch[U(0x123A)].value = b'x'")
    too_long = ex(b, "ch[U(0x1237)].value = bytes(245)")
    ex(b, "ch[U(0x1237)].value = bytes(range(244))\ntime.sleep(0.2)")
    full = extra.written(0x1237)
check(
    "[11] discovery: GAP, GATT and Heart Rate, their characteristics and properties",
    "service UUID(0x1800) True" in found
    and "char UUID(0x2a00) read" in found
    and "service UUID(0x1801)" in found
    and "char UUID(0x2a05) indicate ['UUID(0x2902)']" in found
    and "service UUID(0x180d)" in found
    and "char UUID(0x2a37) notify ['UUID(0x2902)']" in found
    and "char UUID(0x2a38) read []" in found
    and "discovered 3" in found,
    found,
)
check(
    "[11] the client's MTU exchange: the smaller of the program's 247 and the device's 100",
    "MTU_REQ" in trace and "packet 97" in mtu,
    (mtu, trace[-300:]),
)
check(
    "[12] a whitelist: only that service, by Find By Type Value",
    "discovered 1" in only
    and "UUID(0x180d)" in only
    and "FIND_TYPE_REQ" in trace_only
    and "READ_GROUP_REQ" not in trace_only,
    (only, trace_only[-400:]),
)
check("[13] read: the body sensor location", "location b'\\x01'" in read, read)
check(
    "[13] a 300-byte value, by Read Blob",
    "long True 300" in long_read and "READ_BLOB_REQ" in trace_long,
    (long_read, trace_long[-300:]),
)
check("[13] a read not permitted raises", "Read not permitted" in refused, refused)
check(
    "[14] a Write Request and a Write Command reach the device",
    request == [b"request"]
    and command == [b"command"]
    and "WRITE_REQ" in trace_writes
    and "WRITE_CMD" in trace_writes,
    (request, command, trace_writes[-300:]),
)
check("[14] a write to a read-only characteristic raises", "not writable" in read_only, read_only)
check(
    "[14] a write the device refuses raises", "Write not permitted" in write_refused, write_refused
)
check("[14] a value longer than MTU - 3 raises ValueError", "ValueError" in too_long, too_long)
check(
    "[14] a value of MTU - 3 bytes (244) is written whole",
    full[-1:] == [bytes(range(244))],
    [len(v) for v in full],
)

# Notifications and indications, into a CharacteristicBuffer and a PacketBuffer.
with start() as b, cpapi.Air(b) as air:
    air.add(extra)
    hrm = air.add(cpapi.devices.heart_rate())
    ex(b, SETUP)
    ex(b, connect(extra.address))
    ex(b, DISCOVER % "")
    ex(b, "ch = {x.uuid: x for s in services for x in s.characteristics}\nU = _bleio.UUID\n")
    ex(
        b,
        "buf = _bleio.CharacteristicBuffer(ch[U(0x123C)], buffer_size=64)\nch[U(0x123C)].set_cccd(notify=True)",
    )
    cccd = extra.characteristic(0x123C).cccd_value
    trace_cccd = b.follow("trace")[0]
    sent = extra.notify(0x123C, b"tick")
    notified = ex(b, "time.sleep(0.2)\nprint('got', buf.read(buf.in_waiting))")
    ex(
        b,
        "ind = _bleio.CharacteristicBuffer(ch[U(0x123B)], buffer_size=64)\nch[U(0x123B)].set_cccd(indicate=True)",
    )
    indicated = extra.indicate(0x123B, b"ind")
    confirmed = air.wait(lambda: extra.confirmations >= 1)
    got_indication = ex(b, "time.sleep(0.1)\nprint('got', ind.read(ind.in_waiting))")
    ex(b, "c.disconnect()\ntime.sleep(0.2)")
    ex(b, connect(hrm.address))
    ex(b, DISCOVER % "")
    ex(
        b,
        "m = [x for s in services for x in s.characteristics if x.uuid == _bleio.UUID(0x2A37)][0]\n"
        "pb = _bleio.PacketBuffer(m, buffer_size=4)\n",
    )
    pb_cccd = hrm.characteristic(0x2A37).cccd_value
    hrm.beat(88)
    packet = ex(
        b,
        "time.sleep(0.2)\nbuf = bytearray(20)\nn = pb.readinto(buf)\nprint('packet', bytes(buf[:n]))",
    )
check(
    "[15] set_cccd(notify=True) writes 01 00",
    cccd == 1 and "WRITE_REQ" in trace_cccd and "01 00" in trace_cccd,
    (cccd, trace_cccd[-300:]),
)
check(
    "[15] a notification into a CharacteristicBuffer",
    sent and "got b'tick'" in notified,
    (sent, notified),
)
check(
    "[15] an indication, confirmed",
    indicated and confirmed and "got b'ind'" in got_indication,
    (indicated, confirmed, got_indication),
)
check(
    "[15] a PacketBuffer client turns notifications on and gets them",
    pb_cccd == 1 and "packet b'\\x00X'" in packet,
    (pb_cccd, packet),
)

# One service discovered, then another (adafruit_ble's connection[Service] each time): the first
# keeps its notifications. And a 128-bit service.
with start() as b, cpapi.Air(b) as air:
    hrm = air.add(cpapi.devices.heart_rate())
    uart = air.add(cpapi.devices.uart())
    ex(b, SETUP)
    ex(b, connect(hrm.address))
    ex(
        b,
        "m = c.discover_remote_services([_bleio.UUID(0x180D)])[0].characteristics[0]\n"
        "buf = _bleio.CharacteristicBuffer(m, buffer_size=16)\nm.set_cccd(notify=True)\n"
        "gap = c.discover_remote_services([_bleio.UUID(0x1800)])\n",
    )
    hrm.beat(90)
    kept = ex(b, "time.sleep(0.2)\nprint('kept', buf.read(buf.in_waiting))")
    ex(b, "c.disconnect()\ntime.sleep(0.2)")
    ex(b, connect(uart.address))
    long_uuid = ex(b, DISCOVER % "")
check("[15] a second discovery keeps the first's notifications", "kept b'\\x00Z'" in kept, kept)
check(
    "[11] a 128-bit service and its characteristics",
    "service UUID('%s')" % cpapi.NUS_SERVICE in long_uuid
    and "char UUID('%s') write,write_no_response" % cpapi.NUS_RX in long_uuid
    and "char UUID('%s') notify ['UUID(0x2902)']" % cpapi.NUS_TX in long_uuid,
    long_uuid,
)

# The phone's connection: the phone has no services the program asks for.
with start() as b:
    ex(b, PHONE_AND_SENSOR)
    phone = b.ble.connect()
    result = []
    t = threading.Thread(
        target=lambda: result.append(
            ex(
                b,
                "t = time.monotonic()\nprint('phone', a.connections[0].discover_remote_services(), "
                "round(time.monotonic() - t, 1))",
            )
        )
    )
    t.start()
    phone.serve(3)
    t.join(10)
check(
    "[19] discover_remote_services() on the phone's connection: ()",
    result and "phone () " in result[0] and float(result[0].split()[-1]) < 2,
    result,
)

# The device goes in the middle of a read; an answer of no request gets no Error Response.
with start() as b, cpapi.Air(b) as air:
    slow = air.add(
        cpapi.Peripheral(
            "Slow",
            [
                cpapi.GattService(
                    0x1250, [cpapi.GattCharacteristic(0x1251, ("read",), error="none")]
                )
            ],
        )
    )
    ex(b, SETUP)
    ex(b, connect(slow.address))
    ex(b, DISCOVER % "")
    ex(
        b,
        "ch = [x for s in services for x in s.characteristics if x.uuid == _bleio.UUID(0x1251)][0]",
    )
    result = []
    t = threading.Thread(
        target=lambda: result.append(
            ex(
                b,
                "t = time.monotonic()\ntry:\n    ch.value\nexcept Exception as e:\n"
                "    print('raised', type(e).__name__, round(time.monotonic() - t, 1))",
            )
        )
    )
    t.start()
    time.sleep(0.5)
    air.remove(slow)
    t.join(10)
    gone = result[0] if result else None
    hrm = air.add(cpapi.devices.heart_rate())
    ex(b, connect(hrm.address))
    n = len(air.events[0])
    with air.lock:
        hrm._send(bytes.fromhex("17010000"))  # a Prepare Write Response nobody asked for
    time.sleep(0.5)
    with air.lock:
        answers = [
            e for e in air.events[0][n:] if e["kind"] == "l2cap" and e["data"].startswith("01")
        ]
check(
    "[21] the device goes during a read: an error at once, no hang",
    gone is not None and "raised" in gone and float(gone.split()[-1]) < 5,
    gone,
)
check("[22] an unasked response gets no Error Response", answers == [], answers)

# ---- adafruit_ble, as on a CLUE ----

HEART_RATE_CLIENT = """
import time
from adafruit_ble import BLERadio
from adafruit_ble.advertising.standard import ProvideServicesAdvertisement
from adafruit_ble_heart_rate import HeartRateService
ble = BLERadio()
found = None
for adv in ble.start_scan(ProvideServicesAdvertisement, timeout=5):
    if HeartRateService in adv.services:
        found = adv
        break
ble.stop_scan()
print("found", found is not None, found and HeartRateService in found.services)
conn = ble.connect(found)
hr = conn[HeartRateService]
values = None
for i in range(100):
    values = hr.measurement_values
    if values:
        break
    time.sleep(0.05)
print("bpm", values and values.heart_rate)
"""

UART_CLIENT = """
from adafruit_ble import BLERadio
from adafruit_ble.advertising.standard import ProvideServicesAdvertisement
from adafruit_ble.services.nordic import UARTService
ble = BLERadio()
for adv in ble.start_scan(ProvideServicesAdvertisement, timeout=5):
    if UARTService in adv.services:
        break
ble.stop_scan()
uart = ble.connect(adv)[UARTService]
uart.write(b"hello")
print("echo", uart.read(5))
"""

libs = lib_files("adafruit_ble", "adafruit_ble_heart_rate")
if libs is None:
    print(
        "SKIP adafruit_ble: set CIRCUITPY_LIB to a lib with adafruit_ble and adafruit_ble_heart_rate"
    )
else:
    with start(files=libs) as b, cpapi.Air(b) as air:
        hrm = air.add(cpapi.devices.heart_rate())
        beating = threading.Event()

        def beat():
            while not beating.is_set():
                if hrm.connected:
                    hrm.beat(81)
                time.sleep(0.05)

        t = threading.Thread(target=beat, daemon=True)
        t.start()
        out = ex(b, HEART_RATE_CLIENT, timeout=30)
        beating.set()
        t.join()
    check(
        "[5] adafruit_ble: start_scan(ProvideServicesAdvertisement) finds the heart rate device",
        "found True True" in out,
        out,
    )
    check("[16] adafruit_ble: measurement_values gives the bpm", "bpm 81" in out, out)

    with start(files=libs) as b, cpapi.Air(b) as air:
        uart = air.add(cpapi.devices.uart())

        def echo():
            got = uart.received(timeout=15)
            uart.send(got.upper())

        t = threading.Thread(target=echo, daemon=True)
        t.start()
        out = ex(b, UART_CLIENT, timeout=30)
        t.join(5)
    check(
        "[16] adafruit_ble's UART client against cpapi.devices.uart", "echo b'HELLO'" in out, out
    )

# ---- two labs on one air: CLUE to CLUE ----

NUS = cpapi.NUS_SERVICE
ECHO_RADIO = """
from adafruit_ble import BLERadio
from adafruit_ble.advertising.standard import ProvideServicesAdvertisement
from adafruit_ble.services.nordic import UARTService
ble = BLERadio()
uart = UARTService()
ble.start_advertising(ProvideServicesAdvertisement(uart))
print("ready")
was = False
while True:
    if ble.connected != was:
        was = ble.connected
        print("connected" if was else "disconnected")
    if ble.connected and uart.in_waiting:
        uart.write(uart.read(uart.in_waiting))
"""
# The same with _bleio alone, for a lib without adafruit_ble.
ECHO_RAW = """
import _bleio, time
a = _bleio.adapter
a.enabled = True
C = _bleio.Characteristic
s = _bleio.Service(_bleio.UUID(%r))
rx = C.add_to_service(s, _bleio.UUID(%r), properties=C.WRITE | C.WRITE_NO_RESPONSE, max_length=20)
tx = C.add_to_service(s, _bleio.UUID(%r), properties=C.NOTIFY, max_length=20)
buf = _bleio.CharacteristicBuffer(rx, buffer_size=64)
a.start_advertising(bytes([2, 1, 6, 17, 7]) + bytes(_bleio.UUID(%r).uuid128), connectable=True)
print("ready")
was = False
while True:
    if a.connected != was:
        was = a.connected
        print("connected" if was else "disconnected")
    if buf.in_waiting:
        tx.value = buf.read(buf.in_waiting)
    time.sleep(0.01)
""" % (NUS, cpapi.NUS_RX, cpapi.NUS_TX, NUS)
CLIENT_RAW = (
    SETUP
    + """
U = _bleio.UUID(%r)
for e in a.start_scan(timeout=5):
    if e.connectable and bytes(U.uuid128) in bytes(e.advertisement_bytes):
        break
a.stop_scan()
c = a.connect(e.address, timeout=3)
s = c.discover_remote_services([U])[0]
ch = {x.uuid: x for x in s.characteristics}
tx = ch[_bleio.UUID(%r)]
buf = _bleio.CharacteristicBuffer(tx, buffer_size=64)
tx.set_cccd(notify=True)
ch[_bleio.UUID(%r)].value = b"hello"
t = time.monotonic()
while buf.in_waiting < 5 and time.monotonic() - t < 3:
    time.sleep(0.01)
print("echo", buf.read(5))
"""
    % (NUS, cpapi.NUS_TX, cpapi.NUS_RX)
)
CLIENT_RADIO = UART_CLIENT

radio = lib_files("adafruit_ble") is not None
files = lib_files("adafruit_ble") if radio else None
with (
    start("CLUEA", ECHO_RADIO if radio else ECHO_RAW, files=files) as A,
    start("CLUEB", files=files) as B,
):
    A.wait("ready", timeout=15)
    with cpapi.Air(A, B):
        out = ex(B, CLIENT_RADIO if radio else CLIENT_RAW, timeout=30)
        a_connected = seen(A, "connected", 5)
        left = ex(
            B,
            "import _bleio\nfor x in _bleio.adapter.connections:\n    x.disconnect()\nprint('left')",
        )
        a_disconnected = seen(A, "disconnected", 5)
    trace_a, trace_b = A.trace(since=0)["data"], B.trace(since=0)["data"]
    address_a = A.request("GET", "/api/ble")["address"]
    console_a = A.console(since=0)["data"]
check(
    "[17] CLUE to CLUE: B scans, connects, writes hello, reads it back%s"
    % (" (adafruit_ble)" if radio else " (_bleio)"),
    "echo b'hello'" in out,
    out,
)
check(
    "[17] A sees the connection come and go",
    a_connected and a_disconnected,
    (left, console_a[-300:]),
)
connects_a = [x for x in trace_a.splitlines() if "BLE CONNECT " in x]
check(
    "[17] the traces: B central to A, A the peripheral",
    ("BLE CONNECT %s central" % address_a) in trace_b
    and len(connects_a) == 1
    and not connects_a[0].endswith("central"),
    (connects_a, trace_b[-400:]),
)

# Two scanners, one advertiser: three labs on one air.
ADVERTISE = (
    SETUP
    + "a.start_advertising(bytes([2, 1, 6, 5, 9]) + b'Lamp', connectable=True)\nprint('ready')\nwhile True:\n    time.sleep(0.1)\n"
)
with start("LABA", ADVERTISE) as A, start("LABB") as B, start("LABC") as Cc:
    A.wait("ready", timeout=15)
    address_a = A.request("GET", "/api/ble")["address"]
    with cpapi.Air(A, B, Cc):
        ex(B, SETUP)
        ex(Cc, SETUP)
        seen_b = entries(ex(B, SCAN % "timeout=1"))
        seen_c = entries(ex(Cc, SCAN % "timeout=1"))
check(
    "[18] two scanners see one advertiser",
    address_a in [e[0] for e in seen_b] and address_a in [e[0] for e in seen_c],
    (address_a, seen_b, seen_c),
)

# ---- robustness ----

RELOADED = """
import _bleio, gc, time
a = _bleio.adapter
a.enabled = True
addr = _bleio.Address(bytes(reversed(bytes.fromhex(%r))), _bleio.Address.RANDOM_STATIC)
c = a.connect(addr, timeout=3)
gc.collect()
print("connected", c.connected, gc.mem_free())
while True:
    time.sleep(0.1)
"""
with cpapi.Board.launch(
    BIN, drive(RELOADED % "c7:00:00:00:00:01".replace(":", ""), None, "RELOAD"), env=HEADLESS
) as b:
    with cpapi.Air(b) as air:
        hrm = air.add(cpapi.devices.heart_rate(address="c7:00:00:00:00:01"))
        runs = []
        for i in range(6):
            seen(b, "connected", 10)
            seen(b, "\n", 2)
            line = b.console(since=0)["data"].split("connected ")[-1].split()
            linked = hrm.wait_connected(True)
            runs.append(
                (line[0] if line else None, int(line[1]) if len(line) > 1 else None, linked)
            )
            if i < 5:
                b.reload()
                runs[-1] += (hrm.wait_connected(False),)
check(
    "[20] five reloads during a central connection: each ends the link, each run connects",
    len(runs) == 6 and all(r[0] == "True" and r[2] for r in runs) and all(r[3] for r in runs[:5]),
    runs,
)
frees = [r[1] for r in runs[1:] if r[1] is not None]
check(
    "[20] memory within 1 KB over the runs",
    len(frees) == 5 and max(frees) - min(frees) <= 1024,
    frees,
)

# ---- a controller with extended advertising (Zephyr's on an nRF52840) ----

EXTENDED = {"CIRCUITPY_BLE_VIRTUAL_FAULT": "extended"}
EXTENDED_LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "extended.btsnoop")
with start(env=EXTENDED, args=["--ble-log", EXTENDED_LOG]) as b, cpapi.Air(b) as air:
    air.add(cpapi.devices.beacon(IBEACON, rssi=-60, address="c1:22:33:44:55:66"))
    hrm = air.add(cpapi.devices.heart_rate(address="c5:11:22:33:44:55"))
    ex(b, SETUP)
    got = entries(ex(b, SCAN % "timeout=0.5"))
    first = ex(b, connect(hrm.address))
    found = ex(b, DISCOVER % "")
    ex(b, "c.disconnect()\ntime.sleep(0.2)")
    ex(b, "a.start_advertising(bytes([2, 1, 6, 4, 9]) + b'Ext', connectable=True)")
    state = b.request("GET", "/api/ble")
    phone = b.ble.connect()
    name = phone.read(0x2A00)
    trace = b.trace(since=0)["data"]
check(
    "[23] extended: the scan (1)",
    ("c1:22:33:44:55:66", 1, -60, False, False, IBEACON.hex()) in got,
    got,
)
check(
    "[23] extended: a connection (6)",
    first.startswith("connected True 1") and "BLE CONNECT c5:11:22:33:44:55 central" in trace,
    (first, trace[-400:]),
)
check(
    "[23] extended: discovery (11)",
    "service UUID(0x180d)" in found and "discovered 3" in found,
    found,
)
log = open(EXTENDED_LOG, "rb").read()
# H4 commands: 01, the opcode (little-endian); the LE meta event of an Enhanced Connection Complete.
EXTENDED_HCI = (b"\x01\x41\x20", b"\x01\x42\x20", b"\x01\x43\x20", b"\x04\x3e\x1f\x0a")
check(
    "[23] extended: the extended commands and events, no legacy ones",
    all(x in log for x in EXTENDED_HCI)
    and b"\x01\x0c\x20" not in log
    and b"\x01\x0d\x20" not in log,
    len(log),
)
check(
    "[23] extended: advertising, and the phone's connection",
    state["advertising"] and "Ext" in state["name"] and name.startswith(b"CIRCUITPY"),
    (state, name),
)

# ---- MCP: an agent tests a central program ----

SERVER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "cp_mcp.py")


class Mcp:
    """cp_mcp.py over pipes, one request at a time."""

    def __init__(self):
        import subprocess, sys

        self.p = subprocess.Popen(
            [sys.executable, SERVER, "--program", BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            cwd=os.path.dirname(os.path.abspath(__file__)),
        )
        self.n = 0
        self.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}})

    def request(self, method, params):
        import json

        self.n += 1
        self.p.stdin.write(
            json.dumps(
                {"jsonrpc": "2.0", "id": self.n, "method": method, "params": params}
            ).encode()
            + b"\n"
        )
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def call(self, tool, **arguments):
        r = self.request("tools/call", {"name": tool, "arguments": arguments}).get("result", {})
        return "".join(c.get("text", "") for c in r.get("content", [])), bool(r.get("isError"))

    def close(self):
        self.call("stop")
        self.p.stdin.close()
        self.p.wait(10)


m = Mcp()
try:
    tools = [t["name"] for t in m.request("tools/list", {})["result"]["tools"]]
    m.call("launch", drive=drive(IDLE, None, "MCPBLE"))
    added, _ = m.call("ble_device_add", kind="heart_rate", address="c8:00:00:00:00:01", bpm=70)
    m.call("ble_device_add", kind="uart", address="c8:00:00:00:00:02")
    m.call("exec", code=SETUP, interrupt=True)
    out, _ = m.call(
        "exec",
        code=connect("c8:00:00:00:00:02")
        + DISCOVER % ""
        + "ch = {x.uuid: x for s in services for x in s.characteristics}\nU = _bleio.UUID\nrx = ch[U(%r)]\ntx = ch[U(%r)]\nbuf = _bleio.CharacteristicBuffer(tx, buffer_size=64)\ntx.set_cccd(notify=True)\nrx.value = b'ping'\n"
        % (cpapi.NUS_RX, cpapi.NUS_TX),
    )
    written, _ = m.call("ble_device_written", address="c8:00:00:00:00:02", timeout=3)
    sent, _ = m.call("ble_device_notify", address="c8:00:00:00:00:02", text="pong")
    echoed, _ = m.call("exec", code="time.sleep(0.2)\nprint('got', buf.read(buf.in_waiting))")
    removed, _ = m.call("ble_device_remove", address="c8:00:00:00:00:02")
    after, _ = m.call("exec", code="time.sleep(0.2)\nprint('after', c.connected)")
    unknown, unknown_error = m.call("ble_device_notify", address="c8:00:00:00:00:09", bpm=1)
finally:
    m.close()
check(
    "MCP: the ble_device_ tools are there",
    all("ble_device_" + t in tools for t in ("add", "notify", "written", "remove")),
    tools,
)
check("MCP: ble_device_add says where the device is", "c8:00:00:00:00:01" in added, added)
check("MCP: the program connects to the device", "connected True" in out, out)
check("MCP: ble_device_written has what the program wrote", "ping" in written, written)
check(
    "MCP: ble_device_notify reaches the program",
    sent == "sent" and "got b'pong'" in echoed,
    (sent, echoed),
)
check(
    "MCP: ble_device_remove ends the connection",
    "removed" in removed and "after False" in after,
    (removed, after),
)
check("MCP: an unknown device is an error", unknown_error and "no device" in unknown, unknown)

# ---- the panel row ----

with start() as b, cpapi.Air(b) as air:
    hrm = air.add(cpapi.devices.heart_rate(name="Strap"))
    ex(b, SETUP)
    ex(b, "results = a.start_scan()")
    scan_row = b.state().get("ble")
    ex(b, "a.stop_scan()")
    ex(b, connect(hrm.address))
    central_row = b.state().get("ble")
check("the panel row: scanning", scan_row and scan_row.get("state") == "scanning", scan_row)
check(
    "the panel row: connected to the device a scan named",
    central_row
    and central_row.get("state") == "connected"
    and central_row.get("central_to") == "Strap",
    central_row,
)

# ---- cpapi.py air: two labs started by hand, on one air ----

import socket, subprocess, sys  # noqa: E402

TOKEN = "0123456789abcdef0123456789abcdef"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


ports = [free_port(), free_port()]
env = dict(HEADLESS, CIRCUITPY_API_TOKEN=TOKEN)
labs = [
    subprocess.Popen(
        [BIN, "--drive", drive(code, None, name), "--api", str(port)],
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    for code, name, port in ((ADVERTISE, "HANDA", ports[0]), (IDLE, "HANDB", ports[1]))
]
CPAPI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "cpapi.py")
relay = None
try:
    A, B = (cpapi.Board.attach("http://127.0.0.1:%d/" % p, TOKEN) for p in ports)
    A.wait("ready", timeout=15)
    B.wait("idle", timeout=15)
    relay = subprocess.Popen(
        [sys.executable, CPAPI, "air"] + [str(p) for p in ports],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    time.sleep(1)
    address_a = A.request("GET", "/api/ble")["address"]
    ex(B, SETUP)
    by_hand = ex(B, connect(address_a, random=False))
    no_token = subprocess.run(
        [sys.executable, CPAPI, "air", str(ports[0])],
        env=dict(HEADLESS),
        capture_output=True,
        timeout=20,
    )
finally:
    if relay is not None:
        relay.terminate()
        relay.wait(10)
    for p in labs:
        p.kill()
        p.wait()
check(
    "cpapi.py air: B connects to A, both started by hand",
    by_hand.startswith("connected True"),
    by_hand,
)
check("cpapi.py air without the token: 64", no_token.returncode == 64, no_token)

print("DONE")

# The program's notifications go to the phone that subscribed, never to a device it is the
# central of; a phone that leaves takes its subscription along, though the device's link stays.
NOTIFIER = SETUP + (
    "s = _bleio.Service(_bleio.UUID(0x1260))\n"
    "n = _bleio.Characteristic.add_to_service(s, _bleio.UUID(0x1261),\n"
    "    properties=_bleio.Characteristic.NOTIFY | _bleio.Characteristic.READ, max_length=8)\n"
    "a.start_advertising(bytes([2, 1, 6]), connectable=True)\n"
)
with start() as b, cpapi.Air(b) as air:
    hrm = air.add(cpapi.devices.heart_rate())
    ex(b, NOTIFIER)
    phone = b.ble.connect()
    linked = ex(b, connect(hrm.address))
    phone.subscribe(0x1261)
    n0 = len(air.events[0])
    ex(b, "n.value = b'one'\ntime.sleep(0.3)")
    got = phone.notifications(0x1261, 1)
    to_device = [
        e
        for e in air.events[0][n0:]
        if e["kind"] == "l2cap" and e["peer"] == hrm.address and e["data"][:2] in ("1b", "1d")
    ]
    phone.disconnect()
    ex(b, "time.sleep(0.3)\na.start_advertising(bytes([2, 1, 6]), connectable=True)")
    phone2 = b.ble.connect()
    handle = phone2.characteristic(0x1261).handle
    ex(b, "n.value = b'two'\ntime.sleep(0.3)")
    phone2._take(1)
    stale = phone2._notified.get(handle)
check(
    "[18] a notification to the phone that subscribed, none to the device the program connected to",
    "connected True" in linked and got == [b"one"] and to_device == [],
    (linked, got, to_device),
)
check("[18] the next phone has no subscription left by the one that left", not stale, stale)
