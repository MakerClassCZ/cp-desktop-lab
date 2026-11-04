"""Bluetooth LE pairing, bonds and encryption: python3 test_ble_smp.py /path/to/circuitpython

_bleio's SMP over the HCI host stack (LE Legacy "Just Works"), the bond store and its file, and
the virtual controller's encryption, with the phone of tools/cpapi.py pairing as a phone does.
"""

import os, struct, time
from harness import HEADLESS, T, check, cpapi, launch, run, seen

# A command sent from a background task gets its answer: the advertising's own timeout stops it
# (the stop asks the controller from the background tick).
TIMEOUT = """
import _bleio, time
a = _bleio.adapter
a.enabled = True
a.start_advertising(bytes([2, 1, 6]), connectable=True, timeout=1)
print("advertising", a.advertising)
t = time.monotonic()
while a.advertising and time.monotonic() - t < 5:
    time.sleep(0.05)
print("stopped", a.advertising, round(time.monotonic() - t))
"""
out, _ = run(TIMEOUT, "", "stopped", timeout=10)
check(
    "advertising stops at its timeout, from the background",
    "advertising True" in out and "stopped False 1" in out and "Error" not in out,
    out[-400:],
)

# The crypto of LE Legacy pairing on the Bluetooth Core's sample data (Vol 3 Part H 2.2.3 c1,
# 2.2.4 s1, Appendix D.7 ah), in the phone (cpapi, pure Python) and in the host (ble_hci, through
# the lab); AES-128 itself on FIPS-197's example first.
C1 = "1e1e3fef878988ead2a74dc5bef13b86"
S1 = "9a1fe1f0e8b0f49b5b4216ae796da062"
AH = "0dfbaa"
le = cpapi._le
aes = cpapi.aes128(bytes(range(16)), bytes.fromhex("00112233445566778899aabbccddeeff")).hex()
check("cpapi AES-128: FIPS-197's example", aes == "69c4e0d86a7b0430d8cdb78070b4c55a", aes)
c1 = cpapi.smp_c1(
    bytes(16),
    le("5783D52156AD6F0E6388274EC6702EE0"),
    le("07071000000101"),
    le("05000800000302"),
    1,
    le("A1A2A3A4A5A6"),
    0,
    le("B1B2B3B4B5B6"),
)[::-1].hex()
s1 = cpapi.smp_s1(
    bytes(16), le("000F0E0D0C0B0A091122334455667788"), le("010203040506070899AABBCCDDEEFF00")
)[::-1].hex()
ah = cpapi.smp_ah(le("EC0234A357C8AD05341010A60A397D9B"), le("708194"))[::-1].hex()
check("cpapi c1, s1, ah: Core's sample data", (c1, s1, ah) == (C1, S1, AH), (c1, s1, ah))
with launch('print("up")\n') as b:
    seen(b, "up")
    try:
        host = b.request("GET", "/api/ble/crypto")
    except cpapi.ApiError as e:
        host = e
check(
    "ble_hci's c1, s1, ah through the lab: Core's sample data",
    isinstance(host, dict) and (host["c1"], host["s1"], host["ah"]) == (C1, S1, AH),
    host,
)

print("DONE")
