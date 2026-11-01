"""Bluetooth LE: python3 test_ble.py /path/to/circuitpython

_bleio over the HCI host stack: the adapter the port gives every run, --ble, and the virtual
controller with the phone the control API drives.
"""

from harness import check, launch, refused, run, seen

ADAPTER = 'import _bleio\nprint("adapter", _bleio.adapter is not None, _bleio.adapter and _bleio.adapter.enabled)\n'

# The adapter is there before code.py, not yet enabled
out, _ = run(ADAPTER, "", "adapter")
check("_bleio.adapter is there", "adapter True False" in out, out)

# Enabling it runs the HCI init with the virtual controller
out, _ = run(
    'import _bleio\n_bleio.adapter.enabled = True\n'
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
check("--fast-time refuses --ble PORT", status == 2 and "real hardware needs real time" in out, out)

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
        state["advertising"] and state["connectable"] and "020106070943502d6c6162" in state["data"],
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

print("DONE")
