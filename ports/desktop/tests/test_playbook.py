"""The playbook: python3 test_playbook.py /path/to/circuitpython

--playbook FILE plays panel lines, "sleep MS", "wait "TEXT" [MS]", "expect "TEXT"" and
"expect-not "TEXT"" on the program's clock, from the top at each code.py run. Its outcome joins
the run's result.
"""

import os, subprocess, sys
from harness import BIN, T, check, launch

# Beside the tests when they are copied out, else in the port's tools/
CPAPI = os.path.join(T, "cpapi.py")
if not os.path.exists(CPAPI):
    CPAPI = os.path.join(T, "..", "tools", "cpapi.py")

BOARD = '[i2c]\nTEMP = "mcp9808"\n'
PLAYBOOK = os.path.join(T, "test.playbook")

# A write to the sensor every 0.3 s, ten times: which of them a fault met.
PROGRAM = """import board, busio, errno, time
i2c = busio.I2C(board.SCL, board.SDA)
i2c.try_lock()
print("BOOM, before ready")
print("ready")
for i in range(10):
    try:
        i2c.writeto(0x18, bytes([5]))
        print(i, "ok")
    except OSError as e:
        print(i, "ENODEV" if e.errno == errno.ENODEV else e.errno)
    time.sleep(0.3)
print("done")
"""


def play(lines, code=PROGRAM, fast_time=False, runs=1):
    """The outputs and results of runs of code with this playbook."""
    open(PLAYBOOK, "w").write(lines)
    outs, results = [], []
    with launch(code, BOARD, playbook=PLAYBOOK, fast_time=fast_time) as b:
        for i in range(runs):
            if i:
                b.reload()
            outs.append(b.wait("Code done running.", timeout=30))
            results.append(b.result())
        rules = b.faults()["rules"]
    return outs, results, rules


def first_nack(out):
    return next((int(x.split()[0]) for x in out.splitlines() if x.endswith(" ENODEV")), None)


# A sleep on the program's clock: the fault lands at the same write with and without fast time.
TIMED = 'wait "ready"\nsleep 1000\nfault i2c nack addr=0x18 once\nwait "ENODEV"\nexpect-not "Traceback"\n'
for fast in (False, True):
    outs, results, _ = play(TIMED, fast_time=fast)
    check(
        "sleep 1000 then a fault: the write at 1.2 s meets it%s" % (", fast time" if fast else ""),
        first_nack(outs[0]) == 4 and results[0] == {"status": "pass", "detail": ""},
        (first_nack(outs[0]), results, outs[0][-300:]),
    )

# A program that sleeps long: in fast time its clock jumps, and stops at the playbook's sleep.
open(PLAYBOOK, "w").write('wait "ready"\nsleep 1000\npress BTN\n')
# The program's own press marks in the trace when it said ready.
LONG = """import lab, time
lab.panel("press BTN", "release BTN")
print("ready")
time.sleep(3)
print("done")
"""
for fast in (False, True):
    with launch(
        LONG, '[pins]\nBTN = "GPIO5"\n[button]\nBTN = ""\n', playbook=PLAYBOOK, fast_time=fast
    ) as b:
        b.wait("Code done running.", timeout=30)
        pressed = [
            float(x.split()[0])
            for x in b.trace(since=0)["data"].splitlines()
            if "BUTTON BTN pressed" in x
        ]
    check(
        "a press after sleep 1000 comes 1 s after ready while the program sleeps 3 s%s"
        % (", fast time" if fast else ""),
        len(pressed) == 2 and 0.95 < pressed[1] - pressed[0] < 1.3,
        pressed,
    )

outs, results, _ = play('wait "ready"\nexpect-not "BOOM"\nexpect "nope"\n')
check(
    "expect-not looks after the last wait's match; expect fails at its line",
    results[0] == {"status": "fail", "detail": 'playbook line 3: "nope" was not printed'},
    results,
)
outs, results, _ = play('wait "never" 300\n')
check(
    "wait: its limit on the program's clock",
    results[0] == {"status": "fail", "detail": 'playbook line 1: "never" did not come'},
    results,
)
outs, results, _ = play('wait "ready"\nfrobnicate\n')
check(
    "a line that is not a panel line fails the playbook there",
    results[0]["status"] == "fail"
    and results[0]["detail"].startswith("playbook line 2: not a panel command"),
    results,
)
outs, results, _ = play('wait "ready"\nwait "after the end" 60000\n')
check(
    "the run ended while it waited: fail",
    results[0]
    == {
        "status": "fail",
        "detail": 'playbook line 2: the run ended while it waited for "after the end"',
    },
    results,
)
REPORTS = PROGRAM + 'import lab\nlab.result("pass", "the program says so")\n'
outs, results, _ = play('wait "ready"\nexpect "nope"\n', code=REPORTS)
check("a playbook's fail outranks the program's pass", results[0]["status"] == "fail", results)
outs, results, _ = play('wait "ready"\n', code=REPORTS)
check(
    "both pass: the program's result, with its detail",
    results[0] == {"status": "pass", "detail": "the program says so"},
    results,
)

# From the top at each run; its rules end with the run.
outs, results, rules = play('fault i2c nack addr=0x18\nwait "done"\n', runs=2)
check(
    "a reload plays it again from the top",
    all(first_nack(o) == 0 for o in outs) and all(r["status"] == "pass" for r in results),
    (results, [first_nack(o) for o in outs]),
)
check("the playbook's rules end with the run", rules == [], rules)

# ab with a playbook: the result in the table.
open(PLAYBOOK, "w").write(
    'wait "ready"\nfault i2c nack addr=0x18 once\nwait "done"\nexpect "1 ENODEV"\n'
)
path = os.path.join(T, "playbook_code.py")
open(path, "w").write(PROGRAM)
board = os.path.join(T, "playbook_board.toml")
open(board, "w").write(BOARD)
try:
    r = subprocess.run(
        [sys.executable, CPAPI, "ab", BIN, BIN, path, "--names", "one,two", "--runs", "2"]
        + ["--board-file", board, "--playbook", PLAYBOOK],
        capture_output=True,
        text=True,
        timeout=300,
    )
finally:
    for f in (path, board, PLAYBOOK):
        os.remove(f)
check(
    "ab --playbook: its fail is the outcome",
    '| failed | 2/2 `playbook line 4: "1 ENODEV" was not printed`' in r.stdout,
    r.stdout + r.stderr[-500:],
)
