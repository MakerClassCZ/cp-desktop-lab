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

print("DONE")
