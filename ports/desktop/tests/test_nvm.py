"""nvm: python3 test_nvm.py /path/to/circuitpython

microcontroller.nvm is 4 KB kept in a file beside the drive (CIRCUITPY.nvm for CIRCUITPY, or
--nvm PATH): a new one reads 0xFF, as erased flash does, a write is there for the next process,
and the drive itself does not show it.
"""

import os
from harness import T, check, run

NVM = os.path.join(T, "CIRCUITPY.nvm")
OTHER = os.path.join(T, "other.nvm")
for p in (NVM, OTHER):
    if os.path.exists(p):
        os.remove(p)


def data(path):
    return open(path, "rb").read() if os.path.exists(path) else b""


out, _ = run(
    """import microcontroller, os
n = microcontroller.nvm
print("nvm", len(n), n[0], n[4095], n[0:4] == b"\\xff" * 4)
n[0:3] = b"abc"
n[4095] = 7
print("on drive", [f for f in os.listdir("/") if "nvm" in f])
print("written")
""",
    expect="written",
)
check("a new nvm is 4 KB of 0xFF", "nvm 4096 255 255 True" in out, out)
check("the drive does not show the nvm file", "on drive []" in out, out)
d = data(NVM)
check(
    "a write goes to the file beside the drive",
    len(d) == 4096 and d[:3] == b"abc" and d[4095] == 7 and d[3:4095] == b"\xff" * 4092,
    (len(d), d[:4]),
)

out, _ = run(
    """import microcontroller
print("kept", bytes(microcontroller.nvm[0:3]), microcontroller.nvm[4095])
""",
    expect="kept",
)
check("the next process reads what the last one wrote", "kept b'abc' 7" in out, out)

open(OTHER, "wb").write(b"xy")
out, _ = run(
    """import microcontroller
microcontroller.nvm[10] = 1
print("read", bytes(microcontroller.nvm[0:3]))
""",
    expect="read",
    args=("--nvm", OTHER),
)
d = data(OTHER)
check(
    "--nvm PATH: that file, a short one read as 0xFF past its end", "read b'xy\\xff'" in out, out
)
check("--nvm PATH: a write goes there", len(d) >= 11 and d[10] == 1 and d[:2] == b"xy", d[:12])
check("--nvm PATH: the drive's own file untouched", data(NVM)[10] == 0xFF, data(NVM)[:12])

out, _ = run(
    """import microcontroller
try:
    microcontroller.nvm[4096] = 1
except IndexError:
    print("index error")
""",
    expect="index error",
)
check("a write past its end is an IndexError", "index error" in out, out)

for p in (NVM, OTHER):
    os.remove(p)
