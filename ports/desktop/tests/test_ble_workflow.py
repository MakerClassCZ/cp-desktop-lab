"""The BLE workflow: python3 test_ble_workflow.py /path/to/circuitpython

The supervisor's file transfer and serial services (supervisor/shared/bluetooth/) on the virtual
controller, with the control API's phone speaking their protocols (cpapi's FileTransfer and
BleConsole). Until pairing exists the phone's link is not encrypted, so the workflow runs with
--ble-insecure; one check shows what it refuses without it. CIRCUITPY_BLE_BONDED=1 stands in for
a bond. Each scenario is a function: one that raises is a FAIL, and the next one runs.
"""

import os, re, shutil, struct, tempfile, time, traceback
from harness import BIN, HEADLESS, T, check, cpapi, refused, seen

ON = ["--ble-workflow", "on", "--ble-insecure"]
READY = 'print("ready")\nimport time\nwhile True:\n    time.sleep(0.1)\n'
SERVICE = READY.replace(
    'print("ready")',
    "import _bleio\ns = _bleio.Service(_bleio.UUID(0x1234))\n"
    "_bleio.Characteristic.add_to_service(s, _bleio.UUID(0x1235), properties=2, max_length=4)\n"
    'print("ready")',
)
FT, SERIAL, GUARD = "0000febb", "adaf0001", "adaf0004"
BIG = bytes(range(256)) * 12  # 3072 bytes: several reads of 512
FILES = {"lib/neopixel.py": "x = 1\n", "big.bin": BIG, "notes.txt": "hello\n"}
BONDED = dict(HEADLESS, CIRCUITPY_BLE_BONDED="1")
SMALL_BUFFERS = dict(HEADLESS, CIRCUITPY_BLE_VIRTUAL_FAULT="small-buffers")
# The drive in the system's temporary directory: a native file system, where the host's edits are
# seen at once (a drive under /mnt/c in WSL has no inotify).
DRIVE = os.path.join(tempfile.mkdtemp(), "CIRCUITPY")


def launch(code, files=None, args=(), **kw):
    """code.py started on a fresh DRIVE with files ({name: str or bytes}): a cpapi.Board."""
    shutil.rmtree(DRIVE, ignore_errors=True)
    os.makedirs(DRIVE)
    for name, text in dict(files or {}, **{"code.py": code}).items():
        os.makedirs(os.path.dirname(os.path.join(DRIVE, name)), exist_ok=True)
        open(os.path.join(DRIVE, name), "wb" if isinstance(text, bytes) else "w").write(text)
    return cpapi.Board.launch(BIN, DRIVE, args=args, **kw)


def short(u):
    return u.split("-")[0]


def state(b):
    return b.request("GET", "/api/ble")


def until(f, timeout=5):
    """f() once it is true, or its last value after timeout."""
    end = time.time() + timeout
    while True:
        v = f()
        if v or time.time() > end:
            return v
        time.sleep(0.05)


def workflow_is(b, name, timeout=5):
    return until(lambda: state(b).get("workflow") == name, timeout)


def handles(p):
    """The first five services' UUIDs and handle ranges, asked again."""
    p._services = None
    return [(sv.uuid, sv.start, sv.end) for sv in p.services()[:5]]


def host_file(name):
    path = os.path.join(DRIVE, name)
    return open(path, "rb").read() if os.path.exists(path) else None


# ---- A: off by default, on advertises as a CLUE does, its GATT table ----


def off_by_default():
    with launch(READY) as b:
        seen(b, "ready")
        s = state(b)
        scan = b.ble.scan()
    check(
        "off by default: nothing advertised before code.py",
        scan is None and s.get("workflow") == "off",
        s,
    )


def discovery_advertising():
    with launch(READY, args=ON) as b:
        seen(b, "ready")
        ok = workflow_is(b, "discovery")
        s = state(b)
        adv = b.ble.scan()
    check("on, not bonded: discovery", ok and s.get("bonded") is False, s)
    check(
        "discovery: public advertising with flags, 0xFEBB, the Adafruit data, the name",
        adv is not None
        and s["public"]
        and adv.raw[:3] == bytes([2, 1, 6])
        and cpapi.uuid(0xFEBB) in adv.services
        and adv.manufacturer[:2] == b"\x22\x08"
        and struct.unpack("<II", adv.manufacturer[5:13]) == (0x1209, 0x0C)
        and (adv.name or "").startswith("CIRCUITPY")
        and len(adv.name) == 13,
        (adv, s),
    )


def gatt_table():
    with launch(SERVICE, args=ON) as b:
        seen(b, "ready")
        p = b.ble.connect()
        services = p.services()
        order = [short(sv.uuid) for sv in services]
        ft = next(sv for sv in services if short(sv.uuid) == FT)
        props = {short(c.uuid): c.names() for c in ft.characteristics}
        version = p.files.version()
        serial = next(sv for sv in services if short(sv.uuid) == SERIAL)
        serial_props = {short(c.uuid): c.names() for c in serial.characteristics}
    check(
        "GATT: GAP, GATT, file transfer, serial, the guard, then the program's",
        order == ["00001800", "00001801", FT, SERIAL, GUARD, "00001234"],
        order,
    )
    check(
        "file transfer: version 4; 0x0200 read, write without response, notify",
        version == 4
        and props.get("adaf0100") == ["read"]
        and props.get("adaf0200") == ["read", "write_no_response", "notify"],
        (version, props),
    )
    check(
        "serial: 0x0002 written, 0x0003 notifies, 0x0100 the version",
        serial_props
        == {
            "adaf0002": ["write_no_response", "write"],
            "adaf0003": ["notify"],
            "adaf0100": ["read"],
        },
        serial_props,
    )


def needs_encryption():
    # Without --ble-insecure the workflow's characteristics need an encrypted link: none yet.
    with launch(READY, args=["--ble-workflow", "on"]) as b:
        seen(b, "ready")
        p = b.ble.connect()
        try:
            p.subscribe(cpapi.FT_TRANSFER)
            code = None
        except cpapi.AttError as e:
            code = e.code
        version = p.files.version()
    check("without --ble-insecure: the transfer CCCD answers 0x05", code == 0x05, code)
    check("... the version stays open", version == 4, version)


def refusals():
    status, out = refused(["--ble", "off", "--ble-workflow", "on"])
    check("--ble off with the workflow: refused", status == 2 and "needs Bluetooth" in out, out)
    status, out = refused(["--ble", "NOPORT", "--ble-workflow", "on"])
    check(
        "--ble PORT with the workflow, no --ble-insecure: refused before the port opens",
        status == 2 and "--ble-insecure" in out and "cannot open" not in out,
        out,
    )
    status, out = refused(["--ble-workflow", "maybe"])
    check(
        "--ble-workflow maybe: refused", status == 2 and "expected on, discover or off" in out, out
    )


def no_button_wait():
    def ready_time(args):
        start = time.time()
        with launch('print("ready")\n', args=args) as b:
            seen(b, "ready", 10)
            return time.time() - start

    off_t, on_t = ready_time([]), ready_time(ON)
    check("no second's wait for a boot button", on_t < off_t + 0.6, (off_t, on_t))


# ---- B: the file transfer, on a folder and on an image ----


def file_transfer(b, host):
    """The protocol's commands; host(path) is the file's bytes on the drive, or None. Returns
    {check name: ok or (ok, detail)}."""
    f = b.ble.connect().files
    r = {}
    root = {e.name: e for e in f.listdir("/")}
    r["listdir /"] = (
        "code.py" in root
        and root["lib"].directory
        and root["big.bin"].size == len(BIG)
        and not root["big.bin"].directory,
        root,
    )
    lib = [(e.name, e.size) for e in f.listdir("/lib")]
    r["listdir /lib"] = lib == [("neopixel.py", 6)], lib
    r["read, several chunks"] = f.read("/big.bin") == BIG
    data = os.urandom(1500)
    kept = f.write("/new.bin", data, mtime=1700000001 * 10**9)
    r["write a new file"] = host("new.bin") == data
    r["mtime cut to 2 s"] = kept == 1700000000 * 10**9, kept
    f.write("/new.bin", b"short")
    r["overwrite shorter: truncated"] = host("new.bin") == b"short"
    f.write("/empty.txt", b"")
    r["a zero-length write"] = host("empty.txt") == b""
    f.mkdir("/dir/")
    f.mkdir("/dir/sub")
    f.write("/dir/sub/f.txt", b"deep")
    r["mkdir with a trailing /"] = {e.name for e in f.listdir("/dir")} == {"sub"}
    f.move("/notes.txt", "/moved.txt")
    names = {e.name for e in f.listdir("/")}
    r["move"] = (
        "moved.txt" in names and "notes.txt" not in names and host("moved.txt") == b"hello\n"
    )
    f.delete("/dir")
    r["recursive delete"] = "dir" not in {e.name for e in f.listdir("/")}
    try:
        f.listdir("/nothing")
        missing = None
    except cpapi.FileTransferError as e:
        missing = e.status
    r["a missing path: error_no_file"] = missing == "error_no_file", missing
    try:
        f.listdir("/" + "a" * 1100)
        long_error = None
    except cpapi.FileTransferError as e:
        long_error = e.status
    r["a path over 1024 bytes: an error, then the next command works"] = (
        long_error == "error" and "big.bin" in {e.name for e in f.listdir("/")},
        long_error,
    )
    return r


def report(prefix, results):
    for name, value in results.items():
        ok, detail = value if isinstance(value, tuple) else (value, "")
        check(prefix + name, ok, detail)


def folder():
    with launch('print("done")\n', files=FILES, args=ON) as b:
        seen(b, "done")
        report("folder: ", file_transfer(b, host_file))


def image():
    path = os.path.join(T, "workflow.img")
    if os.path.exists(path):
        os.remove(path)
    with cpapi.Board.launch(BIN, path, args=ON) as b:
        seen(b, "\n")
        b.exec(
            "import storage, os\nstorage.remount('/', readonly=False)\n"
            "try:\n    os.mkdir('/lib')\nexcept OSError:\n    pass\n",
            interrupt=True,
        )
        for name, text in FILES.items():
            data = text if isinstance(text, bytes) else text.encode()
            b.exec("with open('/%s', 'wb') as f:\n    f.write(%r)\n" % (name, data))
        b.exec("storage.remount('/', readonly=True)\n")

        def host(name):
            out = b.exec(
                "try:\n    print(open('/%s', 'rb').read().hex())\nexcept OSError:\n    print('none')\n"
                % name
            ).strip()
            return None if out == "none" else bytes.fromhex(out)

        report("image: ", file_transfer(b, host))


def remounted():
    # As on a board: boot.py's remount gives the drive to Python, and a workflow, which holds the
    # filesystem lock while it writes, still writes it (both can; the last one wins).
    remount = 'import storage\nstorage.remount("/", readonly=False)\n'
    with launch('print("done")\n', files={"boot.py": remount}, args=ON) as b:
        seen(b, "done")
        b.ble.connect().files.write("/x.txt", b"x")
    check("boot.py's remount: the phone still writes, as on a board", host_file("x.txt") == b"x")


def paced_read():
    # A read paced to its end closes the file and lets autoreload go on: a host edit reloads.
    with launch('print("started")\n' + READY, files={"big.bin": BIG}, args=ON) as b:
        seen(b, "started")
        whole = b.ble.connect().files.read("/big.bin", chunk=1000) == BIG
        open(os.path.join(DRIVE, "code.py"), "w").write('print("started")\n' + READY)
        reloaded = seen(b, "started", 6)
        out = b.console(since=0)["data"][-300:]
    check("a paced read to its end, then a host edit reloads", whole and reloaded, out)


# ---- C: the MTU ----


def mtu(env=None, name="", expected=247):
    with launch('print("done")\n', files={"big.bin": BIG}, args=ON, env=env) as b:
        seen(b, "done")
        p = b.ble.connect()
        handle = p.characteristic(cpapi.FT_TRANSFER).handle
        sizes = []
        take = p._take

        def counting(timeout):
            got = take(timeout)
            sizes.extend(len(n) for n in p._notified.get(handle, []))
            return got

        p._take = counting
        data = p.files.read("/big.bin")
    check("the MTU is %d%s" % (expected, name), p.mtu == expected, p.mtu)
    check(
        "3 KB read in notifications of up to %d bytes%s" % (expected - 3, name),
        data == BIG and sizes and max(sizes) > 200 and max(sizes) <= expected - 3,
        sorted(set(sizes)),
    )


# ---- D: reloads keep the connection ----

COUNTED = 'import gc\nprint("run", gc.mem_free())\n' + READY.replace('print("ready")\n', "")


def reloads():
    with launch(COUNTED, args=ON) as b:
        seen(b, "run")
        p = b.ble.connect()
        before = handles(p)
        p.files.write("/code.py", COUNTED.replace("run", "run2"))
        reloaded = seen(b, "run2", 8)
        time.sleep(1.5)
        out = b.console(since=0)["data"]
        connected = state(b)["connected"]
        after = handles(p)
        listed = "code.py" in {e.name for e in p.files.listdir("/")}
        frees = []
        for _ in range(5):
            b.reload()
            seen(b, "run2 ", 8)
            b.wait("\n")
            frees.append(int(b.console(since=0)["data"].rsplit("run2 ", 1)[1].split()[0]))
        still = state(b)["connected"] and handles(p) == before
        listed5 = "code.py" in {e.name for e in p.files.listdir("/")}
    check("a write from the phone reloads code.py", reloaded, out[-300:])
    check("... once", out.count("run2") == 1, out[-400:])
    check(
        "... the phone stays connected, the handles stay",
        connected and after == before,
        (connected, before, after),
    )
    check("... and the next command works", listed)
    check("five reloads: connected, the same handles, commands work", still and listed5)
    check("five reloads: the heap does not shrink", max(frees) - min(frees) < 1024, frees)


def between_vms():
    # code.py over, no heap until the next VM: the phone's commands still run.
    with launch('print("done")\n', args=ON) as b:
        seen(b, "done")
        time.sleep(0.5)
        p = b.ble.connect()
        names = {e.name for e in p.files.listdir("/")}
        p.files.write("/code.py", 'print("second")\n')
        second = seen(b, "second", 8)
    check("between VMs: listdir and a write complete", "code.py" in names and second, names)


def program_service():
    # A program with its own service: its reload restarts the stack, as on a CLUE.
    with launch(SERVICE, args=ON) as b:
        seen(b, "ready")
        p = b.ble.connect()
        before = handles(p)
        b.reload()
        seen(b, "ready", 8)
        dropped = until(lambda: not state(b)["connected"])
        again = workflow_is(b, "discovery")
        after = handles(b.ble.connect())
    check("a program's own service: disconnected at its reload", dropped)
    check(
        "... advertises again; the workflow's handles the same",
        again and after == before,
        (before, after),
    )


DISABLES = """import _bleio, supervisor, time
if supervisor.runtime.run_reason == supervisor.RunReason.STARTUP:
    _bleio.adapter.enabled = False
    time.sleep(0.5)
    _bleio.adapter.enabled = True
    print("toggled")
else:
    print("again")
while True:
    time.sleep(0.1)
"""


def program_disables():
    # The program turns the adapter off and on under the workflow: the workflow stops, and is
    # back after the reload.
    with launch(DISABLES, args=ON) as b:
        toggled = seen(b, "toggled", 8)
        b.reload()
        again = seen(b, "again", 8)
        back = workflow_is(b, "discovery")
        names = {e.name for e in b.ble.connect().files.listdir("/")}
        out = b.console(since=0)["data"]
    check(
        "the program turns the adapter off and on: the workflow is back after the reload",
        toggled and again and back and "code.py" in names and "safe mode" not in out,
        out[-400:],
    )


def disconnect_mid_write():
    with launch('print("done")\n', args=ON) as b:
        seen(b, "done")
        p = b.ble.connect()
        f = p.files
        f._send(struct.pack("<BBHIQI", 0x20, 0, 6, 0, 0, 2000) + b"/x.bin")
        f._recv(20)
        p.disconnect()
        readvertised = workflow_is(b, "discovery")
        names = {e.name for e in b.ble.connect().files.listdir("/")}
        open(os.path.join(DRIVE, "code.py"), "w").write('print("edited")\n')
        edited = seen(b, "edited", 6)
    check("a disconnect mid-write: advertising again", readvertised and "code.py" in names, names)
    check("... and autoreload goes on", edited)


def host_edit():
    with launch(READY, args=ON) as b:
        seen(b, "ready")
        p = b.ble.connect()
        open(os.path.join(DRIVE, "code.py"), "a").write("# more\n" * 10)
        reloaded = seen(b, "ready", 6)
        size = os.path.getsize(os.path.join(DRIVE, "code.py"))
        listed = {e.name: e.size for e in p.files.listdir("/")}
    check(
        "a host edit with the phone connected: a reload, the phone's listdir sees the new size",
        reloaded and listed.get("code.py") == size,
        (listed, size),
    )


# ---- E: the REPL over BLE ----


def ble_serial():
    with launch('print("done")\n', args=ON) as b:
        seen(b, "done")
        c = b.ble.connect().console
        c.write("")
        b.write("\r")
        seen(b, ">>> ")
        prompt = until(lambda: c.read(0.2), 3) or ""
        c.write("print(6 * 7)\r")
        out, end = "", time.time() + 5
        while "42" not in out and time.time() < end:
            out += c.read(0.3)
        terminal = seen(b, "42")
    check("BLE serial: the phone has the REPL's prompt", ">>>" in prompt, prompt)
    check("... text from the phone runs, both see the answer", "42" in out and terminal, out)


def ble_ctrl_c():
    with launch('print("looping")\nwhile True:\n    pass\n', args=ON) as b:
        seen(b, "looping")
        b.ble.connect().console.interrupt()
        stopped = seen(b, "KeyboardInterrupt", 5)
        out = b.console(since=0)["data"][-300:]
    check("Ctrl-C from the phone stops a while True", stopped, out)


# ---- F: bonds and discovery ----


def bonded():
    with launch(READY, args=ON, env=BONDED) as b:
        seen(b, "ready")
        ok = workflow_is(b, "reconnecting")
        s = state(b)
        adv = b.ble.scan()
        b.ble.discover()
        seen(b, "ready", 10)
        disc = workflow_is(b, "discovery")
        s2 = state(b)
    check("bonded (the stand-in): private advertising, reconnecting", ok and s["bonded"], s)
    check(
        "... no name, no 0xFEBB",
        adv is not None and adv.name is None and cpapi.uuid(0xFEBB) not in adv.services,
        adv,
    )
    check("POST /api/ble/discover: reset, bonds erased, discovery", disc and not s2["bonded"], s2)


def discover_option():
    with launch(READY, args=["--ble-workflow", "discover", "--ble-insecure"], env=BONDED) as b:
        seen(b, "ready")
        disc = workflow_is(b, "discovery")
        s = state(b)
        b.write("\x03")
        b.write("\r")
        seen(b, ">>> ")
        b.write("import microcontroller; microcontroller.reset()\r")
        seen(b, "ready", 15)
        erased = workflow_is(b, "discovery") and not state(b)["bonded"]
    check(
        "--ble-workflow discover erases the bond, advertises publicly", disc and not s["bonded"], s
    )
    check("... and the bond stays erased after a reset", erased)


def reset_keeps_bond():
    with launch(READY, args=ON, env=BONDED) as b:
        seen(b, "ready")
        b.write("\x03")
        b.write("\r")
        seen(b, ">>> ")
        b.write("import microcontroller; microcontroller.reset()\r")
        seen(b, "ready", 15)
        kept = workflow_is(b, "reconnecting")
        s = state(b)
    check("on: a reset boots as on, the bond kept", kept, s)


# ---- G: the trace and the panel ----


def trace_and_panel():
    with launch('print("done")\n', args=ON) as b:
        seen(b, "done")
        f = b.ble.connect().files
        f.write("/t.txt", b"x" * 412)
        f.listdir("/")
        trace = b.trace(since=0)["data"]
        panel = b.state()["ble"]
    check(
        "the trace: a write and a listdir as the file transfer's commands",
        "BLE FT <- WRITE /t.txt 412 bytes" in trace
        and "BLE FT -> WRITE_PACING ok 0/412" in trace
        and "BLE FT <- WRITE_DATA at 0, 412 bytes" in trace
        and "BLE FT <- LISTDIR /" in trace
        and re.search(r"BLE FT -> LISTDIR_ENTRY \d+/\d+ t\.txt 412", trace)
        and "BLE ATT <- WRITE_CMD" not in trace,
        trace[-1500:],
    )
    check("the panel's BLE row: the workflow's state", panel.get("workflow") == "connected", panel)


def discover_off():
    with launch(READY) as b:
        try:
            b.request("POST", "/api/ble/discover")
            answer = None
        except cpapi.ApiError as e:
            answer = e.status
    check("POST /api/ble/discover with the workflow off: 409", answer == 409, answer)


for scenario in (
    off_by_default,
    discovery_advertising,
    gatt_table,
    needs_encryption,
    refusals,
    no_button_wait,
    folder,
    image,
    remounted,
    paced_read,
    mtu,
    # 8 buffers of 27 bytes hold a frame of 216: the MTU is 212.
    lambda: mtu(SMALL_BUFFERS, ", in 27-byte ACL packets", 212),
    reloads,
    between_vms,
    program_service,
    program_disables,
    disconnect_mid_write,
    host_edit,
    ble_serial,
    ble_ctrl_c,
    bonded,
    discover_option,
    reset_keeps_bond,
    trace_and_panel,
    discover_off,
):
    # TEST_ONLY=name,name runs those scenarios only.
    only = os.environ.get("TEST_ONLY")
    if only and scenario.__name__ not in only.split(","):
        continue
    try:
        scenario()
    except Exception as e:
        check(scenario.__name__, False, "".join(traceback.format_exception_only(type(e), e)))
