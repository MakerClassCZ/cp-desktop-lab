"""cpapi.py ab and --profile: python3 test_ab_profile.py /path/to/circuitpython

ab runs one script on two programs and prints a Markdown table of how the runs ended. --profile
lets the heap grow only as far as a board's: as many 4 KB buffers fit as at that board's REPL.
"""

import os, subprocess, sys
from harness import BIN, T, check, launch

CPAPI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cpapi.py")


def ab(code, *args):
    path = os.path.join(T, "ab_code.py")
    open(path, "w").write(code)
    r = subprocess.run(
        [sys.executable, CPAPI, "ab", BIN, BIN, path, "--names", "one,two"] + list(args),
        capture_output=True,
        text=True,
        timeout=300,
    )
    return r.returncode, r.stdout + r.stderr[-800:]


rc, table = ab('print("fine")\n', "--runs", "2")
check("ab: a table of both", rc == 0 and table.startswith("| ab_code.py | one | two |"), table)
check("ab: every run ended well", "| ok | 2/2 | 2/2 |" in table, table)
check("ab: the last line printed", "| last line | `fine` | `fine` |" in table, table)

rc, table = ab(
    'import board, digitalio\nd = digitalio.DigitalInOut(board.GPIO0)\nraise ValueError("bad")\n',
    "--runs",
    "3",
)
check(
    "ab: an exception counted, with its line",
    "| exception | 3/3 `ValueError: bad` | 3/3 `ValueError: bad` |" in table,
    table,
)
check(
    "ab: no pins left claimed by an object the run dropped",
    "pins left claimed" not in table,
    table,
)

FILL = """import gc
gc.collect()
big = []
try:
    while True:
        big.append(bytearray(4096))
except MemoryError:
    pass
print("fits", len(big))
"""

for profile, low, high in (("rp2040", 35, 39), ("samd51", 28, 32)):
    with launch(FILL, args=["--profile", profile]) as b:
        out = b.wait("fits", timeout=60) + b.wait("\n", timeout=5)
        n = int(out.split("fits")[-1].split()[0])
        check(
            "--profile %s: about as many 4 KB buffers as on the board" % profile,
            low <= n <= high,
            n,
        )

with launch(FILL, args=["--heap", "64"]) as b:
    out = b.wait("fits", timeout=60) + b.wait("\n", timeout=5)
    n = int(out.split("fits")[-1].split()[0])
    check("--heap 64: some 4 KB buffers, fewer than 16", 8 <= n < 16, n)

r = subprocess.run([BIN, "--profile", "z80"], capture_output=True, text=True, timeout=30)
check(
    "an unknown profile is refused", r.returncode == 2 and "no profile z80" in r.stderr, r.stderr
)
print("DONE")
