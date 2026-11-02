"""The panel's gamepad and keys: python3 test_gamepad.py /path/to/circuitpython

A [gamepad] section in the board file puts a handheld's buttons on pins and sets their keys; a
[button] part can have a key of its own; without either a key presses nothing. Played through the
control API ("down KEY", "up KEY", "press NAME", "release NAME"), each step waiting for the program
to report what it saw, and checked in its output and the bus trace.
"""

import cpapi
from harness import check, launch, refused, run

PINS = '[pins]\nSW_A = "GPIO4"\nSW_UP = "GPIO0"\nSW_R = "GPIO11"\nSW_START = "GPIO8"\n'


def gamepad(**keys):
    """A board file with A, UP, R and START on their pins; keys= for the ones given."""
    lines = "".join(
        '%s = "pin=SW_%s%s"\n' % (b, b, " keys=" + keys[b] if b in keys else "")
        for b in ("A", "UP", "R", "START")
    )
    return PINS + "[gamepad]\n" + lines


def watch(changes, names=("SW_A", "SW_UP", "SW_R", "SW_START")):
    """Prints each change of the pins as it comes, then all of them after that many."""
    return """import board, digitalio, time
pins = [digitalio.DigitalInOut(getattr(board, n)) for n in %r]
for p in pins:
    p.pull = digitalio.Pull.UP
seen = []
while len(seen) < %d:
    v = "".join("1" if p.value else "0" for p in pins)
    if not seen or seen[-1] != v:
        seen.append(v)
        print("state", v)
    time.sleep(0.01)
print("seen", seen)
""" % (tuple(names), changes)


def keys(*pairs, first="wait state 1111\n"):
    """Steps: each (command, the state it leads to)."""
    return first + "".join("%s\nwait state %s\n" % pair for pair in pairs)


# The default keys: Z is A, Up is UP, W is R, Return is START
out, trace = run(
    watch(7),
    keys(
        ("down z", "0111"),
        ("up z", "1111"),
        ("down up", "1011"),
        ("up up", "1111"),
        ("down w", "1101"),
        ("up w", "1111"),
    ),
    "seen ",
    gamepad(),
)
check(
    "default keys press A, UP and R on their pins",
    "seen ['1111', '0111', '1111', '1011', '1111', '1101', '1111']" in out,
    out,
)
check(
    "trace: the gamepad's buttons as the keys moved them",
    "GAMEPAD A pressed" in trace
    and "GAMEPAD A released" in trace
    and "GAMEPAD UP pressed" in trace,
    trace,
)

# Keys of one's own: Z no longer A, W taken from R for UP, two keys on UP. Z changes nothing, so
# the first change seen is space's.
out, trace = run(
    watch(7),
    keys(
        ("down space", "0111"),
        ("up space", "1111"),
        ("down w", "1011"),
        ("up w", "1111"),
        ("down i", "1011"),
        ("up i", "1111"),
        first="wait state 1111\ndown z\nup z\n",
    ),
    "seen ",
    gamepad(A="space", UP="w, I"),
)
check(
    "keys from the board file replace the defaults, a key leaves the button it had",
    "seen ['1111', '0111', '1111', '1011', '1111', '1011', '1111']" in out,
    out,
)

# A gamepad button by its name, as the mouse does
out, trace = run(
    watch(3), keys(("press START", "1110"), ("release START", "1111")), "seen ", gamepad()
)
check("press START by name", "seen ['1111', '1110', '1111']" in out, out)
check(
    "trace: START pressed and released",
    "GAMEPAD START pressed" in trace and "GAMEPAD START released" in trace,
    trace,
)

# The bare virtual board has no gamepad: a key presses no pin, a button's name is no part.
with launch(watch(2, ("GPIO4", "GPIO0", "GPIO11", "GPIO8")), None) as b:
    b.wait("state 1111")
    b.panel("down z")
    b.panel("up z")
    try:
        b.panel("press A")
        refused_press = ""
    except cpapi.ApiError as e:
        refused_press = str(e)
    print_out = b.console(since=0)["data"]
check("without a board file the keys press no pin", "state 0111" not in print_out, print_out)
check("without a board file A is no part", "no part A" in refused_press, refused_press)

# A [button] part with a key: Z presses it, the panel's click still does.
out, trace = run(
    watch(3, ("BUTTON",)),
    "wait state 1\ndown z\nwait state 0\nup z\n",
    "seen ",
    '[pins]\nBUTTON = "GPIO4"\n[button]\nBUTTON = "key=Z"\n',
)
check("a button part's key presses it", "seen ['1', '0', '1']" in out, out)
check("trace: the button part pressed by its key", "BUTTON BUTTON pressed" in trace, trace)

# Wrong [gamepad] and [button] lines stop the start, saying why.
for text, why in (
    ('[gamepad]\nA = "Space"\n', 'a [gamepad] button is "pin=PIN keys=KEY, KEY"'),
    ('[gamepad]\nA = "pin=NOWHERE"\n', "no pin NOWHERE"),
    ('[gamepad]\nJUMP = "keys=Space"\n', "no gamepad button JUMP"),
    ('[pins]\nB = "GPIO4"\n[button]\nB = "key=NoSuchKey"\n', "unknown key"),
    (
        PINS + '[gamepad]\nA = "pin=SW_A keys=D"\n[midi]\nKEYS = "keyboard"\n',
        "[gamepad] A: D plays the [midi] keyboard",
    ),
    (
        '[midi]\nKEYS = "keyboard"\n[pins]\nB = "GPIO4"\n[button]\nB = "key=Z"\n',
        "[button] B: Z plays the [midi] keyboard",
    ),
):
    status, said = refused(board=text)
    check("refused: " + why, status not in (None, 0) and why in said, said)

# A shift register of the gamepad's buttons, read as adafruit_pybadger reads a PyBadge's
out, trace = run(
    """import board, keypad, time
keys = keypad.ShiftRegisterKeys(clock=board.BUTTON_CLOCK, data=board.BUTTON_OUT, latch=board.BUTTON_LATCH,
                                key_count=8, value_when_pressed=True)
print("ready")
seen = []
while len(seen) < 4:
    e = keys.events.get()
    if e:
        seen.append((e.key_number, e.pressed))
        print("event", e.key_number, e.pressed)
    time.sleep(0.01)
print("events", seen)
""",
    "wait ready\ndown z\nwait event 1 True\nup z\nwait event 1 False\ndown right\nwait event 4 True\nup right\n",
    "events",
    '[pins]\nBUTTON_OUT = "GPIO20"\nBUTTON_CLOCK = "GPIO21"\nBUTTON_LATCH = "GPIO22"\n'
    '[shiftreg]\nBUTTON_OUT = "clock=BUTTON_CLOCK latch=BUTTON_LATCH keys=B,A,START,SELECT,RIGHT,DOWN,UP,LEFT"\n',
)
check(
    "a shift register shifts out the gamepad's buttons in its order",
    "events [(1, True), (1, False), (4, True), (4, False)]" in out,
    out,
)
print("DONE")
