"""The virtual MIDI keyboard: python3 test_midi.py /path/to/circuitpython

A [midi] section's keyboard and CC sliders reach the program as MIDI bytes on usb_midi.ports[0].
Played through the control API (tap, press, release, down, up, set), each step waiting for the
program to report what it read, and checked in its output, the bus trace and the panel's state.
"""

import cpapi
from harness import check, launch, refused, run

# Prints "ready", then the first N bytes read, in hex.
READER = """import usb_midi, time
port = usb_midi.ports[0]
print("ready")
got = b""
while len(got) < %d:
    data = port.read()
    if data:
        got += data
    time.sleep(0.01)
print("midi", got.hex(" "))
"""
KEYS = '[midi]\nKEYS = "keyboard"\n'

out, trace = run(READER % 6, "wait ready\ntap KEYS C4", "midi ", KEYS)
check("tap KEYS C4: Note On and Off, velocity 100", "midi 90 3c 64 80 3c 00" in out, out)
check(
    "trace: MIDI IN per message",
    "MIDI IN 90 3c 64" in trace and "MIDI IN 80 3c 00" in trace,
    trace,
)

out, _ = run(READER % 3, "wait ready\npress KEYS 61", "midi ", '[midi]\nKEYS = "keyboard channel=2"\n')
check("a number for the note, channel=2", "midi 91 3d 64" in out, out)

out, trace = run('import usb_midi\nprint("none", usb_midi.ports[0].read())\n', "", "none ")
check("no keyboard: read() is None", "none None" in out and "MIDI IN" not in trace, (out, trace))

out, _ = run(
    'import usb_midi, time\nb = bytearray(8)\nprint("ready")\nn = None\n'
    "while not n:\n    n = usb_midi.ports[0].readinto(b)\n    time.sleep(0.01)\n"
    'print("into", n, bytes(b[:n]).hex(" "))\n',
    "wait ready\npress KEYS C3",
    "into ",
    KEYS,
)
check("readinto: count and bytes", "into 3 90 30 64" in out, out)

# A reload empties the queue and releases the held keys on the panel.
with launch(READER % 100, KEYS) as b:
    b.wait("ready")
    b.panel("press KEYS C4")
    b.reload()
    b.wait("ready")
    midi = b.state().get("midi")
    check("after a reload no key is held", midi is not None and midi["held"] == [], midi)
    try:
        b.panel("tap KEYS H9")
        check("bad note refused", False)
    except cpapi.ApiError as e:
        check("bad note refused", "bad note H9" in str(e), e)

status, out = refused(board='[midi]\nA = "keyboard"\nB = "keyboard"\n')
check("second keyboard refused", status == 2 and "one keyboard" in out, (status, out))

# Computer keys, by position: Z is C3 and Q C4; Page Down an octave lower, S its C#.
out, _ = run(READER % 12, "wait ready\ndown z\nup z\ndown q\nup q", "midi ", KEYS)
check("Z plays C3, Q plays C4", "midi 90 30 64 80 30 00 90 3c 64 80 3c 00" in out, out)
out, _ = run(READER % 6, "wait ready\ndown pagedown\nup pagedown\ndown s\nup s", "midi ", KEYS)
check("Page Down: an octave lower; S is C#", "midi 90 25 64 80 25 00" in out, out)
out, _ = run(READER % 6, "wait ready\ndown z\ndown pageup\nup z", "midi ", KEYS)
check("a held note is let go when the octave moves", "midi 90 30 64 80 30 00" in out, out)
# The gamepad gives its letter keys up only on a board with a keyboard.
out, trace = run(READER % 6, "wait ready\ndown z\nup z", "midi ", "[gamepad]\n" + KEYS)
check(
    "with a keyboard Z plays, not the gamepad's A",
    "midi 90 30 64 80 30 00" in out and "GAMEPAD A pressed" not in trace,
    (out, trace),
)
out, trace = run('print("up")\n', "wait up\ndown z\nup z\ndown up\nup up", None, "[gamepad]\n" + KEYS)
check("the gamepad keeps the arrows", "GAMEPAD UP pressed" in trace, trace)

# The panel's state: the keyboard's name, range, channel and held keys; none without one.
with launch(READER % 100, '[midi]\nKEYS = "keyboard C2"\n') as b:
    b.wait("ready")
    b.panel("press KEYS C2", "press KEYS E2")
    s = b.state()["midi"]
    check(
        "state: name, range, channel, held",
        s == {"keyboard": "KEYS", "low": "C2", "high": "C4", "channel": 1, "held": ["C2", "E2"]},
        s,
    )
with launch('print("up")\n') as b:
    b.wait("up")
    check("state: no keyboard", b.state()["midi"] is None, b.state())

# CC sliders: each one's value on the run's first read, then each change.
CCS = '[midi]\nCUTOFF = "cc 74"\nRESO = "cc 71 0"\n'
out, _ = run(READER % 6, "wait ready", "midi ", CCS)
check("first read: each CC's value", "midi b0 4a 40 b0 47 00" in out, out)
out, trace = run(READER % 9, "wait ready\nset CUTOFF 100", "midi ", CCS)
check("set moves the slider and sends", "b0 4a 64" in out and "MIDI IN b0 4a 64" in trace, (out, trace))
out, _ = run(READER % 9, "wait ready\nset CUTOFF 64\nsleep 1000", "midi ", CCS, timeout=2)
check("the same value sends nothing new", "midi " not in out, out)
out, _ = run(READER % 3, "wait ready", "midi ", '[midi]\nMOD = "cc 1 127 channel=3"\n')
check("a starting value and a channel", "midi b2 01 7f" in out, out)
with launch(READER % 100, CCS) as b:
    b.wait("ready")
    b.panel("set CUTOFF 300")
    cc = [p for p in b.state()["parts"] if p["name"] == "CUTOFF"][0]
    check("state: a CC slider, kept in 0-127", cc == {"name": "CUTOFF", "kind": "cc", "cc": 74, "channel": 1, "value": 127}, cc)
status, out = refused(board="[midi]\n" + "".join('C%d = "cc %d"\n' % (i, i) for i in range(9)))
check("a ninth CC slider refused", status == 2 and "8 CC" in out, (status, out))

# From Python: board.parts.Keyboard and board.parts.CC, attached, read, set and detached.
with launch('print("up")\n') as b:
    b.wait("up")
    b.exec(
        "import board\nk = board.attach(board.parts.Keyboard(low='C2'))\n"
        "c = board.attach(board.parts.CC(74, name='CUTOFF'))\n"
    )
    names = b.eval("sorted(board.attached)")
    check("attached lists them", "'CUTOFF'" in names and "'KEYS'" in names, names)
    b.exec("c.value = 100")
    cc = [p for p in b.state()["parts"] if p["name"] == "CUTOFF"][0]
    check("CC value set from Python moves the slider", cc["value"] == 100, cc)
    check("a CC's value read back", b.eval("c.value") == "100", b.eval("c.value"))
    check("repr", "<Keyboard KEYS: C2-C4" in b.eval("repr(k)"), b.eval("repr(k)"))
    b.panel("press KEYS D2")
    check("held and low", b.eval("(k.low, k.held)") == "('C2', ['D2'])", b.eval("(k.low, k.held)"))
    r = b.run("board.attach(board.parts.Keyboard(name='K2'))")
    check("a second keyboard refused", "ValueError" in str(r.get("error")), r)
    b.exec("board.detach(k)\nboard.detach('CUTOFF')")
    check("detached: no keyboard in state", b.state()["midi"] is None, b.state()["midi"])
    check("detached by name: no CC slider", not [p for p in b.state()["parts"] if p["name"] == "CUTOFF"], b.state()["parts"])
    check("the catalogue lists them", "CC" in b.eval("dir(board.parts)") and "Keyboard" in b.eval("dir(board.parts)"))
