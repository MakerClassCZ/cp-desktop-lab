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
        "['attach', 'attached', 'audio', 'claimed', 'detach', 'exit', 'fired', 'heap', 'info', 'panel', "
        "'parts', 'result', 'trace']" in out,
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
    # What the program sees of the lab is what cpapi sees.
    out = b.exec(
        "import gc\n"
        "mark = lab.trace()[1]\n"
        "lab.panel('press B5', 'release B5')\n"
        "text, after = lab.trace(mark)\n"
        "print('trace', 'BUTTON B5 pressed' in text, 'BUTTON B5 released' in text, after > mark)\n"
        "print('claimed', lab.claimed())\n"
        "class Leak:\n    pass\n"
        "kept = [Leak() for _ in range(50)]\n"
        "gc.collect()\n"
        "h = lab.heap()\n"
        "print('heap', h['types']['Leak'], h['free'] > 0, h['largest_free'] <= h['free'], h['used'] > 0)\n"
        "i = lab.info()\n"
        "print('info', i['board'], i['profile'], i['pid'], i['machine'] != '')"
    )
    check(
        "lab.trace(offset): the lines since, and the next offset",
        "trace True True True" in out,
        out,
    )
    claimed = b.claimed()
    check(
        "lab.claimed() is cpapi's claimed()",
        "claimed %r" % claimed in out and len(claimed) == 1,
        (out, claimed),
    )
    heap = b.heap()
    check(
        "lab.heap(): a class's instances counted under its name, as cpapi's heap()",
        "heap [50, " in out and heap["types"]["Leak"][0] == 50,
        (out, heap.get("types", {}).get("Leak")),
    )
    info = b.info()
    check(
        "lab.info() is cpapi's info()",
        "info virtual None %d True" % info["pid"] in out and info["profile"] is None,
        (out, info),
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

# The heap's sizes in bytes: used is gc.mem_alloc() (but for the global it went into), and the
# largest free block, after a collect in a fresh heap, is more than a sixteenth of the heap (in
# blocks of 16 bytes it never could be).
with launch('print("ready")\n', args=["--profile", "rp2040"]) as b:
    b.wait("Code done running.", timeout=15)
    out = b.exec(
        "import gc, lab\n"
        "gc.collect()\n"
        "used = gc.mem_alloc()\n"
        "h = lab.heap()\n"
        "print('sizes', 0 <= h['used'] - used < 1024, h['largest_free'] * 16 > h['total'],"
        " h['largest_free'] <= h['free'] < h['total'])"
    )
    check("lab.heap(): used, free and largest_free in bytes", "sizes True True True" in out, out)

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
