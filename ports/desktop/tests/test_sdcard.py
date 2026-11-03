"""An SD card on the virtual SPI bus: python3 test_sdcard.py /path/to/circuitpython

sdcardio.SDCard finds a card the board file names ([spi] SD = "sdcard cs=PIN"), or one put on its
chip select by itself; it comes formatted, storage mounts it, and what is written there is read
back after it is mounted again.
"""

import os
from harness import T, check, launch, run, seen

# The card is an image beside the drive; each run here starts with a new one, as a drive does.
IMAGE = os.path.join(T, "CIRCUITPY-SD.img")
if os.path.exists(IMAGE):
    os.remove(IMAGE)
# The mount point, as a new drive has it.
SD_DIR = {"sd/placeholder.txt": ""}

USE = """import board, os, sdcardio, storage
sd = sdcardio.SDCard(board.SPI(), board.%s)
vfs = storage.VfsFat(sd)
storage.mount(vfs, "/sd")
with open("/sd/log.txt", "w") as f:
    f.write("hello card")
storage.umount("/sd")
storage.mount(storage.VfsFat(sd), "/sd")
print("read", open("/sd/log.txt").read(), sd.count() * 512 // (1024 * 1024), "MB", os.listdir("/sd"))
"""

out, trace = run(
    USE % "SD_CS",
    expect="read",
    files=SD_DIR,
    board='[pins]\nSD_CS = "GPIO10"\n[spi]\nSD = "sdcard cs=SD_CS"\n',
)
check(
    "a board file's SD card: mounted, written, read back",
    "read hello card 16 MB ['log.txt']" in out,
    out,
)
check(
    "trace: the card's block reads and writes",
    "SD read" in trace and "SD write" in trace,
    trace[-400:],
)

os.remove(IMAGE)
with launch(USE % "D10", '[pins]\nD10 = "GPIO10"\n', files=SD_DIR) as b:
    seen(b, "read", 15)
    out, said = b.console(since=0)["data"], "".join(b.stderr)
check("an SD card put on the chip select by itself", "read hello card" in out, out)
check("and that said on stderr", "SD card attached at D10 (automatic" in said, said)
print("DONE")
