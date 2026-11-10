"""The lab module: python3 test_lab.py /path/to/circuitpython

lab is the program's view of the lab: parts, panel lines, the run's result and the process's end.
board has the board's pins and buses only, as on a board.
"""

import os, subprocess, sys
from harness import BIN, T, check, launch

# Beside the tests when they are copied out, else in the port's tools/
CPAPI = os.path.join(T, "cpapi.py")
if not os.path.exists(CPAPI):
    CPAPI = os.path.join(T, "..", "tools", "cpapi.py")

with launch('print("ready")\n') as b:
    b.wait("Code done running.", timeout=15)
    out = b.exec(
        "import board, lab\n"
        "print([n for n in ('attach', 'detach', 'attached', 'parts', 'exit') if hasattr(board, n)])\n"
        "print(sorted(n for n in dir(lab) if not n.startswith('_')))"
    )
    check("board has none of the lab's names", "[]" in out.splitlines()[0], out)
    check(
        "lab has them",
        "['attach', 'attached', 'detach', 'exit', 'fired', 'panel', 'parts', 'result']" in out,
        out,
    )
    out = b.exec(
        "import digitalio\n"
        "lab.attach(lab.parts.Button(board.GPIO5, name='B5'))\n"
        "d = digitalio.DigitalInOut(board.GPIO5)\n"
        "d.pull = digitalio.Pull.UP\n"
        "up = d.value\n"
        "lab.panel('press B5')\n"
        "print('pressed', up, d.value)\n"
        "lab.panel('release B5')\n"
        "print('released', d.value)"
    )
    check(
        "lab.panel presses a part as the API does",
        "pressed True False" in out and "released True" in out,
        out,
    )
    out = b.exec(
        "try:\n    lab.panel('frobnicate B5')\nexcept ValueError as e:\n    print('err', e)"
    )
    check(
        "lab.panel: ValueError naming the line",
        "err not a panel command: frobnicate B5" in out,
        out,
    )
    out = b.exec("try:\n    lab.result('maybe')\nexcept ValueError as e:\n    print('err', e)")
    check("lab.result: pass, fail or skip only", "err status is" in out, out)

# The result: the run's, from the run that reported it; gone in a run that does not.
REPORT = """import lab, supervisor
if supervisor.runtime.run_reason == supervisor.RunReason.STARTUP:
    lab.result("fail", "first run")
print("reported")
"""
with launch(REPORT) as b:
    b.wait("Code done running.", timeout=15)
    first = b.result()
    b.reload()
    b.wait("Code done running.", timeout=15)
    second = b.result()
    check(
        "a run's result is in the after-run report",
        first == {"status": "fail", "detail": "first run"},
        first,
    )
    check("the next run's is its own: none reported", second is None, second)


def cpapi(*args, code):
    path = os.path.join(T, "lab_code.py")
    open(path, "w").write(code)
    try:
        r = subprocess.run(
            [sys.executable, CPAPI] + list(args) + [path],
            capture_output=True,
            text=True,
            timeout=300,
        )
    finally:
        os.remove(path)
    return r.returncode, r.stdout + r.stderr[-800:]


rc, out = cpapi("run", BIN, code='import lab\nlab.result("fail", "too slow")\n')
check(
    "cpapi run: a reported fail is printed, exit status 4",
    rc == 4 and "--- result: fail: too slow" in out,
    (rc, out[-600:]),
)
rc, out = cpapi("run", BIN, code='import lab\nlab.result("pass")\n')
check("cpapi run: a pass, exit status 0", rc == 0 and "--- result: pass" in out, (rc, out[-600:]))

# ab: the reported result is the outcome, unless the run ended in an exception.
rc, out = cpapi(
    "ab",
    BIN,
    BIN,
    "--names",
    "one,two",
    "--runs",
    "2",
    code='import lab\nlab.result("pass")\nraise OSError(5)\n',
)
check(
    "ab: an exception outranks a reported pass",
    rc == 0 and "| exception | 2/2" in out and "passed" not in out,
    out,
)
rc, out = cpapi(
    "ab",
    BIN,
    BIN,
    "--names",
    "one,two",
    "--runs",
    "2",
    code='import lab\nlab.result("fail", "late")\n',
)
check(
    "ab: a reported fail is a row of its own", "| failed | 2/2 `late` | 2/2 `late` |" in out, out
)
