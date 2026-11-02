"""--code FILE: python3 test_code.py /path/to/circuitpython

A file on the drive run in place of code.py, from its own directory, for every run of the
process; cpapi's launch (code_file) and run (--code) take it too.
"""

import os, subprocess, sys
from harness import BIN, HEADLESS, check, cpapi, drive, launch, refused, seen

FILES = {
    "games/snake.py": 'import os, helper\nprint("snake from", os.getcwd(), helper.NAME)\n',
    "games/helper.py": 'NAME = "the helper beside it"\n',
}

with launch('print("code.py ran")\n', files=FILES, code_file="games/snake.py") as b:
    first = seen(b, "snake from /games the helper beside it")
    b.reload()
    again = seen(b, "snake from")
    out = b.console(since=0)["data"]
check("--code runs the file, from its directory", first, out[-400:])
check("a reload runs it again, not code.py", again and "code.py ran" not in out, out[-400:])

status, out = refused(["--code", "games/none.py"])
check(
    "a file that is not on the drive stops the start",
    status == 2 and "--code games/none.py" in out,
    (status, out[-300:]),
)

# cpapi run: a drive directory and the file in it
d = drive('print("code.py ran")\n', FILES, name="CODE_DRIVE")
CPAPI = os.path.join(os.path.dirname(os.path.abspath(cpapi.__file__)), "cpapi.py")
p = subprocess.run(
    [sys.executable, CPAPI, "run", BIN, d, "--code", "games/snake.py", "--wait", "snake from"],
    capture_output=True,
    timeout=60,
    env=HEADLESS,
)
out = (p.stdout + p.stderr).decode(errors="replace")
check(
    "cpapi run --code",
    p.returncode == 0 and "snake from /games" in out,
    (p.returncode, out[-300:]),
)

print("DONE")
