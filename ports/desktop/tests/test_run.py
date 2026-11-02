"""cpapi.py run, one run of a script from the command line: python3 test_run.py /path/to/circuitpython"""

import os, subprocess, sys, tempfile
from harness import BIN, HEADLESS, check, cpapi, picture

CPAPI = os.path.join(os.path.dirname(os.path.abspath(cpapi.__file__)), "cpapi.py")
W = tempfile.mkdtemp()


def run(code, *args, timeout=90):
    """cpapi.py run on code.py: (exit status, what it printed)."""
    path = os.path.join(W, "code.py")
    open(path, "w").write(code)
    p = subprocess.run(
        [sys.executable, CPAPI, "run", BIN, path] + list(args),
        capture_output=True,
        timeout=timeout,
        env=HEADLESS,
    )
    return p.returncode, (p.stdout + p.stderr).decode(errors="replace")


RED = """import board, displayio
bitmap = displayio.Bitmap(board.DISPLAY.width, board.DISPLAY.height, 1)
palette = displayio.Palette(1); palette[0] = 0xff0000
group = displayio.Group(); group.append(displayio.TileGrid(bitmap, pixel_shader=palette))
board.DISPLAY.root_group = group
board.DISPLAY.refresh()
print("drawn")
while True:
    pass
"""
png = os.path.join(W, "shot.png")
rc, out = run(RED, "--wait", "drawn", "--colors", "--png", png)
check("--colors: the pixels per colour", rc == 0 and "0xff0000 76800" in out, (rc, out[-400:]))
saved = open(png, "rb").read() if os.path.exists(png) else b""
p = cpapi.Picture.from_png(saved) if saved else None
check("--png: the display saved", p is not None and p[5, 5] == 0xFF0000, (rc, len(saved)))

rc, out = run("1/0\n")
check(
    "code.py that ends in an exception: status 3, its traceback",
    rc == 3 and "ZeroDivisionError" in out,
    (rc, out[-300:]),
)

rc, out = run('import board\nprint("leaving")\nboard.exit(3)\n')
check(
    "a program that ends itself: status 1, its exit status, no traceback of run's",
    rc == 1 and "leaving" in out and "the program ended" in out and "Traceback" not in out,
    (rc, out[-400:]),
)

p = subprocess.run([sys.executable, CPAPI, "run", BIN], capture_output=True, env=HEADLESS)
check(
    "run without CODE: a usage error, status 64",
    p.returncode == 64,
    (p.returncode, p.stderr[-200:]),
)

rc, out = run('print("never")\nwhile True:\n    pass\n', "--wait", "nope", "--timeout", "1")
check("--wait for text that never comes: status 1", rc == 1 and "never" in out, (rc, out[-300:]))

rc, out = run('print("each run")\n', "--runs", "3")
check(
    "a script that ends, --runs 3: status 0, each run's console",
    rc == 0 and out.count("--- run ") == 3 and out.count("each run") == 3,
    (rc, out[-500:]),
)
check("the window's title escapes are left out", "\x1b]0;" not in out, repr(out[:300]))

CRASH = """import displayio, terminalio
font = terminalio.FONT
w, h = font.get_bounding_box()
grid = displayio.TileGrid(font.bitmap, pixel_shader=displayio.ColorConverter(),
                          width=20, height=5, tile_width=w, tile_height=h)
terminalio.Terminal(grid, font).write("\\x1b[31mred")
"""
rc, out = run(CRASH)
check(
    "a hard fault: status 2, the backtrace for the symbolizer",
    rc == 2 and "hard fault" in out and "backtrace for " in out and "\n  0x" in out,
    (rc, out[-800:]),
)
