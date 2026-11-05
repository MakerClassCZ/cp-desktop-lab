"""Bluetooth LE pairing, bonds and encryption: python3 test_ble_smp.py /path/to/circuitpython

_bleio's SMP over the HCI host stack (LE Legacy "Just Works"), the bond store and its file, and
the virtual controller's encryption, with the phone of tools/cpapi.py pairing as a phone does.
"""

import os, struct, time
from harness import BIN, HEADLESS, T, check, cpapi, launch, run, seen

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

# A frame on an L2CAP channel the program does not have: an L2CAP Command Reject (invalid CID) on
# the signaling channel; a signaling request it does not serve: Command Reject (not understood).
ADVERTISE = """
import _bleio, time
a = _bleio.adapter
a.start_advertising(bytes([2, 1, 6]), connectable=True)
print("ready")
while True:
    time.sleep(0.05)
"""
with launch(ADVERTISE) as b:
    seen(b, "ready")
    b.request("POST", "/api/ble/connect")
    try:
        b.request("POST", "/api/ble/l2cap", {"cid": 0x0041, "data": "0102"})
        unknown = b.request("GET", "/api/ble/l2cap?wait=2000")["frames"]
        # LE Credit Based Connection Request, identifier 7.
        b.request("POST", "/api/ble/l2cap", {"cid": 5, "data": "14070a00800001000000000000"})
        signal = b.request("GET", "/api/ble/l2cap?wait=2000")["frames"]
    except cpapi.ApiError as e:
        unknown = signal = e
check(
    "an unknown channel: Command Reject, invalid CID",
    isinstance(unknown, list) and unknown == [{"cid": 5, "data": "01000600020041000000"}],
    unknown,
)
check(
    "a signaling request not served: Command Reject, not understood",
    isinstance(signal, list) and signal == [{"cid": 5, "data": "010702000000"}],
    signal,
)

# The program for the pairing tests: characteristics of each security mode; it says when its
# link's encryption changes, and advertises again after a disconnect. A phone's write of a word
# to the OPEN command characteristic makes it do something: pair, pair without a bond, erase.
SECURE = """
import _bleio, time
a = _bleio.adapter
A, C = _bleio.Attribute, _bleio.Characteristic
s = _bleio.Service(_bleio.UUID(0x1240))
RW = C.READ | C.WRITE
enc = C.add_to_service(s, _bleio.UUID(0x1241), properties=RW | C.NOTIFY,
    read_perm=A.ENCRYPT_NO_MITM, write_perm=A.ENCRYPT_NO_MITM, max_length=8, initial_value=b"secret")
mitm = C.add_to_service(s, _bleio.UUID(0x1242), properties=RW,
    read_perm=A.ENCRYPT_WITH_MITM, write_perm=A.ENCRYPT_WITH_MITM, max_length=8, initial_value=b"mitm")
hidden = C.add_to_service(s, _bleio.UUID(0x1245), properties=RW,
    read_perm=A.NO_ACCESS, write_perm=A.NO_ACCESS, max_length=8, initial_value=b"hidden")
cmd = C.add_to_service(s, _bleio.UUID(0x1246), properties=RW | C.NOTIFY,
    read_perm=A.OPEN, write_perm=A.OPEN, max_length=8, initial_value=b"-")
ADV = bytes([2, 1, 6])
a.start_advertising(ADV, connectable=True)
print("ready", a.address)
paired = None
while True:
    cs = a.connections
    p = bool(cs) and cs[0].paired
    if p != paired:
        paired = p
        print("paired", p)
    if not a.connected and not a.advertising:
        a.start_advertising(ADV, connectable=True)
    word = bytes(cmd.value)
    if word != b"-" and cs:
        cmd.value = b"-"
        c = cs[0]
        try:
            if word == b"pair":
                c.pair()
            elif word == b"pairnb":
                c.pair(bond=False)
            elif word == b"erase":
                a.erase_bonding()
            elif word == b"notify":
                enc.value = b"enc"
                cmd.value = b"-"
                cmd.value = b"tick"
            elif word == b"confirm":
                c.confirm_pairing(True)
            print("done", word.decode(), c.paired, c.authenticated, c.numeric_comparison)
        except Exception as e:
            print("raised", word.decode(), type(e).__name__, e)
    time.sleep(0.02)
"""

TRACE = [
    "BLE PAIR <- REQUEST io=none bond key=16",
    "BLE PAIR -> RESPONSE io=none bond key=16",
    "BLE PAIR <- CONFIRM",
    "BLE PAIR -> CONFIRM",
    "BLE PAIR <- RANDOM",
    "BLE PAIR -> RANDOM",
    "BLE ENCRYPTED key=16",
    "BLE PAIR -> ENCRYPTION_INFORMATION",
    "BLE PAIR -> IDENTITY_INFORMATION",
    "BLE PAIR -> IDENTITY_ADDRESS 02:dc:00:00:00:01",
    "BLE PAIR <- IDENTITY_INFORMATION",
    "BLE BOND saved identity",
]


def in_order(text, lines):
    """Whether each of lines comes in text, in that order; the first missing one otherwise."""
    at = 0
    for line in lines:
        at = text.find(line, at)
        if at < 0:
            return line
        at += len(line)
    return True


def pair_error(f):
    """The reason of the PairingFailed or the status of the EncryptError f raises, or its result."""
    try:
        return ("returned", f())
    except cpapi.PairingFailed as e:
        return ("failed", e.reason)
    except cpapi.EncryptError as e:
        return ("encrypt", e.status)


# A phone with an identity pairs and bonds (LE Legacy, Just Works); what the program and the
# lab see of it.
with launch(SECURE) as b:
    seen(b, "ready")
    phone = cpapi.Identity()
    p = b.ble.connect(identity=phone)
    bond = p.pair()
    paired = seen(b, "paired True")
    state = b.ble.state()
    bonds = b.ble.bonds()
    trace = b.trace(since=0)["data"]
    read = p.read(0x1241)
    out = b.console(since=0)["data"]
check("the program sees connection.paired", paired, out[-300:])
check(
    "the phone's bond: the program's LTK, EDIV, Rand, IRK and identity",
    bond.ltk is not None
    and len(bond.ltk) == 16
    and bond.ltk != bytes(16)
    and bond.ediv != 0
    and bond.irk is not None
    and bond.identity == "02:dc:00:00:00:01"
    and bond.identity_type == 0
    and bond.key_size == 16
    and bond.phone is phone,
    vars(bond),
)
check(
    "/api/ble: encrypted, key 16, one bond",
    state["encrypted"]
    and state["key_size"] == 16
    and state["bonds"] == 1
    and state["links"][0]["encrypted"],
    state,
)
check(
    "/api/ble/bonds: the phone's identity, with its IRK",
    len(bonds) == 1
    and bonds[0]["peer"] == cpapi.address_text(phone.address)
    and bonds[0]["irk"]
    and bonds[0]["role"] == "peripheral"
    and bonds[0]["own"] == "02:dc:00:00:00:01"
    and bonds[0]["key_size"] == 16,
    bonds,
)
check(
    "the trace: the pairing's lines in order",
    in_order(trace, TRACE) is True,
    (in_order(trace, TRACE), trace[-1500:]),
)
check("the trace shows no key", bond.ltk.hex() not in trace.replace(" ", ""), trace[-800:])
check("an ENCRYPT_NO_MITM read after pairing", read == b"secret", read)

# Pairing that fails: a wrong confirm value, a key too short; a key of 7; a wrong LTK.
with launch(SECURE) as b:
    seen(b, "ready")
    p = b.ble.connect(auto_pair=False)
    tampered = pair_error(lambda: p.pair(tamper="confirm"))
    after_tamper = b.ble.bonds()
    short = pair_error(lambda: p.pair(max_key_size=6))
    seven = p.pair(max_key_size=7)
    state = b.ble.state()
    trace = b.trace(since=0)["data"]
    p.disconnect()
    seen(b, "paired False")
    p = b.ble.connect(auto_pair=False)
    wrong = pair_error(lambda: p.encrypt(seven, ltk=bytes(16)))
    gone = seen(b, "paired False", 2) or not b.ble.state()["connected"]
check(
    "a wrong Mconfirm: Pairing Failed 0x04, no bond",
    tampered == ("failed", 4) and after_tamper == [],
    (tampered, after_tamper),
)
check("the trace says why", "BLE PAIR FAILED 0x04 confirm value failed" in trace, trace[-600:])
check("a phone's maximum key size of 6: Pairing Failed 0x06", short == ("failed", 6), short)
check(
    "a maximum of 7: paired with a 7-byte key",
    seven.key_size == 7 and seven.ltk[7:] == bytes(9) and state["key_size"] == 7,
    (vars(seven), state),
)
check(
    "a wrong LTK from the phone: MIC failure, the link ends",
    wrong == ("encrypt", 0x3D) and gone,
    wrong,
)

# Without a bond: encrypted, no keys given, nothing stored.
with launch(SECURE) as b:
    seen(b, "ready")
    p = b.ble.connect(auto_pair=False, identity=cpapi.Identity())
    nb = p.pair(bond=False)
    encrypted = p.encrypted
    bonds = b.ble.bonds()
    exists = os.path.exists(os.path.join(T, "CIRCUITPY.bonds"))
check(
    "pair(bond=False) on the phone: encrypted, no keys",
    encrypted and nb.ltk is None and nb.irk is None,
    vars(nb),
)
check("nothing stored", bonds == [] and not exists, (bonds, exists))

# The bond file: CIRCUITPY.bonds beside the drive, kept across a restart of the lab; a bonded phone
# comes back from a new resolvable private address and encrypts with its key, found by EDIV and
# Rand; a known peer is asked to encrypt (Security Request), and gets 0x0F, not 0x05.
BONDS = os.path.join(T, "CIRCUITPY.bonds")


def relaunch(code=None, args=(), env=None):
    """The program again on the drive of the last launch, its bonds file kept."""
    if code is not None:
        open(os.path.join(T, "CIRCUITPY", "code.py"), "w").write(code)
    return cpapi.Board.launch(BIN, os.path.join(T, "CIRCUITPY"), args=args, env=env)


def att_error(f):
    """The ATT error code f raises, or what it returned."""
    try:
        return ("returned", f())
    except cpapi.AttError as e:
        return e.code


with launch(SECURE) as b:
    seen(b, "ready")
    phone = cpapi.Identity()
    p = b.ble.connect(identity=phone)
    discovered = [c.uuid for s in p.services() for c in s.characteristics]
    first_read = att_error(lambda: p.read(0x1241))  # auto_pair: 0x05, paired, read again
    bond = p.bond
    mitm = att_error(lambda: p.read(0x1242))
    hidden = (att_error(lambda: p.read(0x1245)), att_error(lambda: p.write(0x1245, b"x")))
    # Notifications of the encrypted characteristic on: kept in the bond.
    p.subscribe(0x1241)
    time.sleep(1)
    p.disconnect()
    seen(b, "paired False")
saved = os.path.exists(BONDS)
check("discovery on a link not yet encrypted", cpapi.uuid(0x1241) in discovered, discovered)
check(
    "auto_pair: one read pairs and reads",
    first_read == ("returned", b"secret") and bond is not None,
    first_read,
)
check("ENCRYPT_WITH_MITM after Just Works: still 0x05", mitm == 0x05, mitm)
check("NO_ACCESS: 0x02, 0x03", hidden == (0x02, 0x03), hidden)
check("CIRCUITPY.bonds beside the drive", saved)

with relaunch() as b:
    seen(b, "ready")
    # The phone, not encrypting: it is known (its address resolves with its IRK).
    p = b.ble.connect(identity=phone, auto_pair=False)
    requests = p.security_requests(timeout=2)
    known = att_error(lambda: p.read(0x1241))
    # Now it encrypts with its bond, as asked.
    p.encrypt(bond)
    paired = seen(b, "paired True")
    read = p.read(0x1241)
    # The CCCD it turned on before the restart is back: a notification without subscribing.
    enc = p.characteristic(0x1241)
    p.write(0x1246, b"notify")
    seen(b, "done notify")
    p._take(1)
    notified = p._notified.get(enc.handle)
    p.disconnect()
    seen(b, "paired False")
    unknown = b.ble.connect(auto_pair=False)
    unknown_requests = unknown.security_requests(timeout=1)
    unknown_read = att_error(lambda: unknown.read(0x1241))
    trace = b.trace(since=0)["data"]
check("a known peer gets a Security Request on connecting", requests == [1], requests)
check("a known peer that has not encrypted: 0x0F", known == 0x0F, known)
check(
    "the bond survived the restart: encrypted with it",
    paired and read == b"secret",
    (paired, read),
)
check(
    "the trace: the LTK found by its EDIV",
    "BLE LTK ediv=0x%04x found" % bond.ediv in trace,
    trace[-800:],
)
check("no pairing on the reconnect", trace.count("BLE PAIR <- REQUEST") == 0, trace[-800:])
check(
    "the bonded peer's CCCD came back: notified without subscribing",
    notified == [b"enc"],
    (notified, p._notified),
)
check(
    "an unknown peer: no Security Request, 0x05",
    unknown_requests == [] and unknown_read == 0x05,
    (unknown_requests, unknown_read),
)

# No notification of an encrypted characteristic before the link is encrypted, CCCD or not.
with relaunch() as b:
    seen(b, "ready")
    p = b.ble.connect(identity=phone, auto_pair=False)
    enc = p.characteristic(0x1241)
    p.write(0x1246, b"notify")
    seen(b, "done notify")
    p._take(1)
    before = p._notified.pop(enc.handle, None)
    p.encrypt(bond)
    p.write(0x1246, b"notify")
    seen(b, "done notify")
    p._take(1)
    after = p._notified.pop(enc.handle, None)
check(
    "no notification before encryption, one after",
    before is None and after == [b"enc"],
    (before, after),
)

# The program changed its services: on the bonded reconnect Service Changed is indicated for
# every handle, and the old CCCDs are not put back.
with relaunch() as b:
    seen(b, "ready")
    p = b.ble.connect(bond=bond)
    sc = p.characteristic(0x2A05)
    p._request(b"\x12" + struct.pack("<HH", sc.cccd, 2), 0x13)
    time.sleep(1)
    p.disconnect()
    seen(b, "paired False")
MORE = SECURE.replace(
    "s = _bleio.Service(_bleio.UUID(0x1240))",
    'x = _bleio.Service(_bleio.UUID(0x1230))\nC.add_to_service(x, _bleio.UUID(0x1231), properties=C.READ, initial_value=b"x")\ns = _bleio.Service(_bleio.UUID(0x1240))',
)
with relaunch(MORE) as b:
    seen(b, "ready")
    p = b.ble.connect(bond=bond)
    p._take(1)
    indicated = dict(p._notified)
    enc = p.characteristic(0x1241)
    p.write(0x1246, b"notify")
    seen(b, "done notify")
    p._take(1)
    stale = p._notified.get(enc.handle)
check(
    "services changed: Service Changed 0x0001-0xFFFF indicated",
    indicated.get(sc.handle) == [bytes.fromhex("0100ffff")],
    indicated,
)
check("... and the old CCCDs not put back", stale is None, stale)

# erase_bonding(): the bonds of this identity go; the phone's next encryption gets Key Missing;
# it pairs again and a new bond is made. A phone that forgot pairs again: the bond replaced.
with relaunch() as b:
    seen(b, "ready")
    p = b.ble.connect(identity=phone, auto_pair=False)
    p.write(0x1246, b"erase")
    seen(b, "done erase")
    after_erase = b.ble.bonds()
    missing = pair_error(lambda: p.encrypt(bond))
    again = p.pair()
    after_again = b.ble.bonds()
    p.disconnect()
    seen(b, "paired False")
    p = b.ble.connect(identity=phone, auto_pair=False)
    forgot = p.pair()
    after_forgot = b.ble.bonds(keys=True)
    trace = b.trace(since=0)["data"]
check("erase_bonding(): no bond of this identity", after_erase == [], after_erase)
check("the trace says so", "BLE BOND erased 1" in trace, trace[-600:])
check("the phone's encryption then: Key Missing", missing == ("encrypt", 6), missing)
check(
    "... and the trace: LTK not found",
    "BLE ENCRYPT FAILED 0x06 key missing" in trace,
    trace[-600:],
)
check("it pairs again: a new bond", len(after_again) == 1 and again.ltk != bond.ltk, after_again)
check(
    "a phone that forgot pairs again: still one bond, the new key",
    len(after_forgot) == 1
    and after_forgot[0]["ltk"] == forgot.ltk.hex()
    and forgot.ltk != again.ltk,
    after_forgot,
)

# A ninth bond takes the place of the least recently used.
with launch(SECURE) as b:
    seen(b, "ready")
    phones = [cpapi.Identity() for _ in range(9)]
    for ph in phones:
        p = b.ble.connect(identity=ph, auto_pair=False)
        p.pair()
        p.disconnect()
        seen(b, "paired False")
    peers = [x["peer"] for x in b.ble.bonds()]
check(
    "nine phones: eight bonds, the first one's gone",
    len(peers) == 8
    and cpapi.address_text(phones[0].address) not in peers
    and all(cpapi.address_text(ph.address) in peers for ph in phones[1:]),
    peers,
)

# --ble-bonds moves the file (relative to the start directory, as --nvm).
other = os.path.join(T, "other.bonds")
for path in (other, BONDS):
    if os.path.exists(path):
        os.remove(path)
with relaunch(args=["--ble-bonds", "other.bonds"]) as b:
    seen(b, "ready")
    b.ble.connect(identity=cpapi.Identity(), pair=True)
check("--ble-bonds: the bonds in that file", os.path.exists(other) and not os.path.exists(BONDS))
if os.path.exists(other):
    os.remove(other)

# A controller with neither a public nor a static address of its own: the host's static address,
# made once, is kept with the bonds: the same on the next start.
NAME = 'import _bleio\n_bleio.adapter.enabled = True\nprint("address", bytes(_bleio.adapter.address.address_bytes).hex(), "!")\n'
fault = dict(HEADLESS, CIRCUITPY_BLE_VIRTUAL_FAULT="no-static-address")
with launch(NAME, env=fault) as b:
    seen(b, "address")
    first = b.console(since=0)["data"]
with relaunch(env=fault) as b:
    seen(b, "address")
    second = b.console(since=0)["data"]
addresses = [x.split("address ", 1)[1].split()[0] for x in (first, second) if "address " in x]
check(
    "no static address of its own: the same one on two starts",
    len(addresses) == 2 and addresses[0] == addresses[1],
    (first[-200:], second[-200:]),
)

# Connection.pair() as the peripheral: a Security Request; the phone pairs, or encrypts with its
# bond; SecurityError when the phone fails it, or ignores it for SMP's 30 s (fast time here).
with launch(SECURE) as b:
    seen(b, "ready")
    phone = cpapi.Identity()
    p = b.ble.connect(identity=phone, auto_pair=False)
    p.write(0x1246, b"pair")
    requests = p.security_requests(timeout=2)
    bond = p.pair()
    done = seen(b, "done pair True False None")
    p.disconnect()
    seen(b, "paired False")
    p = b.ble.connect(identity=phone, auto_pair=False)
    p.security_requests(timeout=1)
    p.write(0x1246, b"pair")
    again = p.security_requests(timeout=2)
    p.encrypt(bond)
    reencrypted = seen(b, "done pair True")
    p.write(0x1246, b"confirm")
    confirm = seen(b, "raised confirm BluetoothError No pairing in progress")
    p.disconnect()
    seen(b, "paired False")
    p = b.ble.connect(auto_pair=False)
    p.write(0x1246, b"pair")
    p.security_requests(timeout=2)
    pair_error(lambda: p.pair(tamper="confirm"))
    tampered = seen(b, "raised pair SecurityError Pairing failed: confirm value failed (0x04)")
    p.disconnect()
    seen(b, "paired False")
    p = b.ble.connect(auto_pair=False, identity=cpapi.Identity())
    p.write(0x1246, b"pairnb")
    nb_requests = p.security_requests(timeout=2)
    nb = p.pair()
    nb_done = seen(b, "done pairnb True")
    bonds = b.ble.bonds()
    out = b.console(since=0)["data"]
check("pair(): a Security Request asking for a bond", requests == [1], requests)
check("pair(): the phone pairs, pair() returns paired, not authenticated", done, out[-400:])
check(
    "pair() on a bonded phone: it only encrypts", again == [1] and reencrypted, (again, out[-300:])
)
check("confirm_pairing(): BluetoothError, nothing pending", confirm, out[-300:])
check("pair() and a wrong confirm value: SecurityError", tampered, out[-300:])
check(
    "pair(bond=False): a request with no bond, encrypted, nothing stored",
    nb_requests == [0] and nb_done and nb.ltk is None and len(bonds) == 1,
    (nb_requests, vars(nb), bonds),
)

with launch(SECURE, fast_time=True) as b:
    seen(b, "ready")
    p = b.ble.connect(auto_pair=False)
    p.write(0x1246, b"pair")
    timed_out = seen(b, "raised pair SecurityError Pairing timed out", 20)
    out = b.console(since=0)["data"]
check(
    "pair() and a phone that ignores it: SecurityError after the SMP timeout",
    timed_out,
    out[-300:],
)

# --ble off: the phone's endpoints answer 409, the bond store is there.
with launch('print("up")\n', args=["--ble", "off"]) as b:
    seen(b, "up")
    answers = []
    for method, path, body in (
        ("GET", "/api/ble/smp", None),
        ("POST", "/api/ble/smp", "0b01"),
        ("POST", "/api/ble/encrypt", {"ltk": "00" * 16, "ediv": 1, "rand": "00" * 8}),
    ):
        try:
            b.request(method, path, body)
            answers.append(200)
        except cpapi.ApiError as e:
            answers.append(e.status)
    bonds = b.ble.bonds()
check("--ble off: the phone's SMP endpoints answer 409", answers == [409, 409, 409], answers)
check("--ble off: /api/ble/bonds answers", bonds == [], bonds)

# A BLE keyboard (adafruit_ble's HIDService, ENCRYPT_NO_MITM): the phone pairs as it reads the
# report map, gets key reports, and after a reconnect the next ones without subscribing again.
LIB = os.environ.get("CIRCUITPY_LIB")
HID = """
import time
from adafruit_ble import BLERadio
from adafruit_ble.advertising.standard import ProvideServicesAdvertisement
from adafruit_ble.services.standard.hid import HIDService
hid = HIDService()
ble = BLERadio()
adv = ProvideServicesAdvertisement(hid)
ble.start_advertising(adv)
print("advertising")
keyboard = hid.devices[0]
n, was = 0, False
while True:
    if ble.connected != was:
        was = ble.connected
        print("connected" if was else "disconnected")
        if not was:
            ble.start_advertising(adv)
    if was:
        n += 1
        keyboard.send_report(bytes([0, 0, 4 + n % 26, 0, 0, 0, 0, 0]))
    time.sleep(0.1)
"""
libs = {}
if LIB and os.path.isdir(os.path.join(LIB, "adafruit_ble")):
    for root, _, names in os.walk(os.path.join(LIB, "adafruit_ble")):
        for name in names:
            path = os.path.join(root, name)
            libs["lib/" + os.path.relpath(path, LIB).replace(os.sep, "/")] = open(
                path, "rb"
            ).read()
if not libs:
    print("SKIP HID keyboard: set CIRCUITPY_LIB to a lib with adafruit_ble")
else:
    with launch(HID, files=libs) as b:
        seen(b, "advertising", 15)
        phone = cpapi.Identity()
        p = b.ble.connect(identity=phone)
        report_map = p.read(0x2A4B)
        # The keyboard's input report: the first report with notifications.
        reports = [
            c for s in p.services() for c in s.characteristics if c.uuid == cpapi.uuid(0x2A4D)
        ]
        report = next(c for c in reports if c.cccd is not None)
        p._request(b"\x12" + struct.pack("<HH", report.cccd, 1), 0x13)
        p._take(1)
        keys = p._notified.pop(report.handle, [])
        p.disconnect()
        seen(b, "disconnected")
        p = b.ble.connect(bond=p.bond)
        p._take(1)
        later = p._notified.get(report.handle, [])
    check("HID: the report map read after pairing", len(report_map) > 20, report_map)
    check("HID: key reports", len(keys) > 0 and all(len(k) == 8 for k in keys), keys)
    check("HID: after a reconnect, reports without subscribing again", len(later) > 0, later)

print("DONE")
