import os, subprocess
from harness import BIN, HEADLESS, Run, T, check, drive, launch, refused, run, seen

# an unknown board name lists the known ones and exits 2
status, out = refused(["--board", "nope"])
check(
    "unknown board message",
    status == 2 and "unknown board nope; known: virtual, ft232h, mcp2221, gpiochip" in out,
    "status %r %s" % (status, out),
)

# --list-boards: every board, whether its adapter is plugged in; nothing else starts
lb = subprocess.run(
    [BIN, "--list-boards"],
    cwd=T,
    stdin=subprocess.DEVNULL,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    env=HEADLESS,
    timeout=15,
)
lines = lb.stdout.decode(errors="replace").replace("\r", "").strip().splitlines()
names = [l.split("\t")[0] for l in lines]
states = [l.split("\t")[1] for l in lines if "\t" in l]
check(
    "list boards",
    lb.returncode == 0
    and names == ["virtual", "ft232h", "mcp2221", "gpiochip"]
    and lines[0] == "virtual\talways"
    and all(s in ("connected", "not connected") for s in states[1:]),
    repr(lb.stdout),
)

# the virtual board is the default: its names, the virtual buses on any pins, all 28 GPIO numbers,
# and sys.platform naming the host as CPython does (libraries test it to tell boards apart)
console, _ = run(
    """import board, busio, microcontroller, sys
print("board", board.board_id, hasattr(board, "SW_A"), hasattr(board, "D0"))
busio.SPI(board.SW_A, board.SW_B, board.SW_X)
busio.I2C(board.SW_Y, board.SW_L)
print("buses built")
print("mcu pins", hasattr(microcontroller.pin, "GPIO27"), hasattr(microcontroller.pin, "GPIO28"))
print("platform", sys.platform)
""",
    expect="platform",
)
check("virtual board_id and names", "board virtual True False" in console, console)
check(
    "the virtual board's SPI and I2C are virtual buses, on any pins",
    "buses built" in console,
    console,
)
check("microcontroller.pin GPIO0-27", "mcu pins True False" in console, console)
check("sys.platform", "platform " + ("win32" if os.name == "nt" else "linux") in console, console)

# board.exit() ends the program with its status, from code.py and from the REPL
status, out = refused(code='import board\nprint("leaving")\nboard.exit(3)\nprint("still here")\n')
check(
    "board.exit(3) from code.py",
    status == 3 and "leaving" in out and "still here" not in out,
    "status %r %s" % (status, out),
)
r = Run(["--drive", drive('print("to the repl")\n')], stdin=subprocess.PIPE)
r.wait_for("Press any key")
r.p.stdin.write(b"\r\nimport board\r\nboard.exit()\r\n")
r.p.stdin.flush()
try:
    status = r.p.wait(timeout=10)
except subprocess.TimeoutExpired:
    status = None
r.stop()
check("board.exit() from the REPL", status == 0, "status %r %s" % (status, r.text()[-300:]))

# Tab in the REPL completes a pin name that only the board file gives
with launch('print("to the repl")\n', board='[pins]\nZQ_LAMP = "GPIO5"\n') as b:
    b.wait("Press any key")
    b.write("\r")
    b.wait(">>> ")
    b.write("import board\rboard.ZQ_L\t")
    check(
        "Tab completes a board file's pin",
        seen(b, "board.ZQ_LAMP"),
        b.console(since=0)["data"][-300:],
    )

# --board gpiochip needs a GPIO chip of the kernel: without one (Windows, WSL) it says why and exits 2
status, out = refused(["--board", "gpiochip", "--gpiochip", "/dev/nonexistent-gpiochip"])
want = "runs on Linux only" if os.name == "nt" else "cannot open /dev/nonexistent-gpiochip"
check(
    "gpiochip board without a GPIO chip",
    status == 2 and want in out,
    "status %r %s" % (status, out),
)

# --board gpiochip where a GPIO chip is (a Pi): a bus device that is not there says so in the exception
if os.path.exists("/dev/gpiochip0"):
    D = drive("""import board, busio
for make in (lambda: busio.I2C(board.SCL, board.SDA), lambda: busio.SPI(board.SCK, board.MOSI, board.MISO)):
    try:
        make()
    except OSError as e:
        print("refused:", e)
print("buses done")
""")
    r = Run(["--board", "gpiochip", "--i2c-bus", "99", "--spi", "/dev/spidev9.9", "--drive", D])
    r.wait_for("buses done", 15)
    r.stop()
    check(
        "gpiochip board: the missing I2C and SPI devices named in the exceptions",
        "refused: cannot open /dev/i2c-99: No such file or directory" in r.text()
        and "refused: cannot open /dev/spidev9.9: No such file or directory" in r.text(),
        r.text(),
    )
    boot_out = open(os.path.join(D, "boot_out.txt"), errors="replace").read()
    check(
        "gpiochip board: boot_out.txt names the chosen board",
        "Board ID:gpiochip" in boot_out,
        boot_out,
    )
else:
    print("SKIP no /dev/gpiochip0 for --board gpiochip")
print("DONE")
