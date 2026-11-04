"""Bluetooth LE, the program as central: python3 test_ble_central.py /path/to/circuitpython

The virtual controller's air: devices the test plays (cpapi.Peripheral, cpapi.devices) that the
program scans for, connects to and reads as a GATT client; and two programs on one air (CLUE to
CLUE) through cpapi.Air's relay.
"""

import os, time
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

print("DONE")
