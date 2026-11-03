"""microcontroller.watchdog: python3 test_watchdog.py /path/to/circuitpython

RAISE: a program that stops feeding it gets watchdog.WatchDogTimeout; RESET: it resets the board,
code.py runs again from the start. Fed in time, neither happens.
"""

from harness import check, run

out, _ = run(
    """import microcontroller, time, watchdog
w = microcontroller.watchdog
w.timeout = 0.5
w.mode = watchdog.WatchDogMode.RAISE
try:
    for i in range(5):
        w.feed()
        time.sleep(0.2)
    print("fed in time")
    time.sleep(2)
    print("not raised")
except watchdog.WatchDogTimeout:
    print("timeout raised")
""",
    expect="raised",
)
check("fed in time, the watchdog waits", "fed in time" in out, out)
check(
    "unfed, RAISE raises WatchDogTimeout", "timeout raised" in out and "not raised" not in out, out
)

out, _ = run(
    """import microcontroller, os, time, watchdog
if "reset.flag" in os.listdir("/"):
    print("ran again after the reset")
else:
    open("/reset.flag", "w").close()
    w = microcontroller.watchdog
    w.timeout = 0.5
    w.mode = watchdog.WatchDogMode.RESET
    print("waiting")
    time.sleep(3)
    print("not reset")
""",
    expect="ran again",
    files={"boot.py": 'import storage\nstorage.remount("/", readonly=False)\n'},
    timeout=20,
)
check(
    "unfed, RESET resets the board",
    "ran again after the reset" in out and "not reset" not in out,
    out,
)
print("DONE")
