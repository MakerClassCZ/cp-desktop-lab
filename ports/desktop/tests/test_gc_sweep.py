"""The GC's sweep after an in-place growth: python3 test_gc_sweep.py /path/to/circuitpython

bytes() of a generator grows its buffer in place up to the end of a small heap area; the sweep
then read one block past the area and ran a "finaliser" from foreign memory (objects with
finalisers make the sweep look). It showed in about a third of the runs, so six run side by side,
each on a drive of its own, for six seconds.
"""

import os, subprocess, time
from harness import BIN, HEADLESS, T, check, drive

CODE = """objs = [open("code.py"), open("code.py")]
grid = [(4, 5, 6)] * 64
while True:
    bytes(c for r, g, b in grid for c in (g, r, b))
"""
runs = []
for i in range(6):
    d = drive(CODE, name="CIRCUITPY_GC%d" % i)
    log = open(os.path.join(T, "gc_out%d.txt" % i), "wb")
    runs.append(
        (
            subprocess.Popen(
                [BIN, "--drive", d, "--ram", "2"],
                cwd=T,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                env=HEADLESS,
            ),
            log,
        )
    )
time.sleep(6)
faults, last = 0, ""
for p, log in runs:
    p.kill()
    p.wait(timeout=5)
    log.close()
    text = open(log.name, "rb").read().decode(errors="replace")
    if "hard fault" in text:
        faults += 1
        last = text
check(
    "generator bytes over a growing heap", faults == 0, "%d of 6 runs faulted: %s" % (faults, last)
)
print("DONE")
