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


rc, out = run('print("hello from run")\n')
check("a script that ends: its console, status 0", rc == 0 and "hello from run" in out, (rc, out))
check("the window's title escapes are left out", "\x1b]0;" not in out, repr(out[:300]))

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

rc, out = run('print("never")\nwhile True:\n    pass\n', "--wait", "nope", "--timeout", "2")
check("--wait for text that never comes: status 1", rc == 1 and "never" in out, (rc, out[-300:]))

rc, out = run('print("each run")\n', "--runs", "3")
check(
    "--runs 3: code.py three times in one process, each run's console",
    rc == 0 and out.count("--- run ") == 3 and out.count("each run") == 3,
    (rc, out[-500:]),
)

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
