"""Bluetooth LE, the program as central: python3 test_ble_central.py /path/to/circuitpython

The virtual controller's air: devices the test plays (cpapi.Peripheral, cpapi.devices) that the
program scans for, connects to and reads as a GATT client; and two programs on one air (CLUE to
CLUE) through cpapi.Air's relay.
"""

import os, threading, time
from harness import BIN, HEADLESS, check, cpapi, drive, lib_files

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

LONG = bytes(range(60))
with start() as b, cpapi.Air(b) as air:
    hrm = air.add(cpapi.devices.heart_rate(address="c6:00:00:00:00:01"))
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
        b, "print('long', ch[U(0x1235)].value == bytes(range(60)), len(ch[U(0x1235)].value))"
    )
    trace_long = b.follow("trace")[0]
    refused = ex(b, "ch[U(0x1236)].value")
    ex(b, "ch[U(0x1237)].value = b'request'\nch[U(0x1238)].value = b'command'\ntime.sleep(0.2)")
    request, command = extra.written(0x1237), extra.wait_written(0x1238)
    trace_writes = b.follow("trace")[0]
    read_only = ex(b, "ch[U(0x1239)].value = b'x'")
    write_refused = ex(b, "ch[U(0x123A)].value = b'x'")
    too_long = ex(b, "ch[U(0x1237)].value = bytes(21)")
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
    "[11] the client's MTU exchange: the smaller of 23 and the device's 247",
    "MTU_REQ" in trace and "packet 20" in mtu,
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
    "[13] a 60-byte value, by Read Blob",
    "long True 60" in long_read and "READ_BLOB_REQ" in trace_long,
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

print("DONE")
