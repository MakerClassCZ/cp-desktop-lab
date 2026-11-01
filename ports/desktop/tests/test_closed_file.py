"""A closed file: python3 test_closed_file.py /path/to/circuitpython

A directory drive answers as the FAT drive of a board does, so a read that fails during a refresh
does not raise out of it and leave the display dead.
"""

import os, struct
from harness import T, check, launch, picture, cpapi, BIN, seen

OPS = """
f = open("/code.py", "rb")
f.close()
for name, op in (("read", lambda: f.read(1)), ("readinto", lambda: f.readinto(bytearray(1))),
                 ("readline", f.readline), ("write", lambda: f.write(b"x")), ("flush", f.flush),
                 ("close", f.close)):
    try:
        op()
        print(name, "no error")
    except Exception as e:
        print(name, type(e).__name__, e.errno if isinstance(e, OSError) else "")
"""

with launch('print("up")\n') as b:
    seen(b, "up")
    on_dir = b.exec(OPS, interrupt=True)

image = os.path.join(T, "closed.img")
if os.path.exists(image):
    os.remove(image)
with cpapi.Board.launch(BIN, image) as b:
    on_fat = b.exec(OPS, interrupt=True)
os.remove(image)
check("read fails with OSError 22", "read OSError 22" in on_dir, on_dir)
check("a directory drive answers as a FAT drive", on_dir == on_fat, on_dir + on_fat)

# More bitmaps than the 32 mappings, so the last one reads its closed file in the refresh.
BMP = (
    b"BM"
    + struct.pack("<IHHI", 54 + 16, 0, 0, 54)
    + struct.pack("<IiiHHIIiiII", 40, 2, 2, 1, 24, 0, 16, 0, 0, 0, 0)
    + b"\xff\xff\xff" * 2
    + b"\0\0"
    + b"\xff\xff\xff" * 2
    + b"\0\0"
)
CODE = """
import board, displayio
d = board.DISPLAY
d.auto_refresh = False
with open("/w.bmp", "rb") as f:
    bitmaps = [displayio.OnDiskBitmap(f) for _ in range(33)]
last = bitmaps[-1]
g = displayio.Group()
g.append(displayio.TileGrid(last, pixel_shader=last.pixel_shader))
d.root_group = g
try:
    print("refresh", d.refresh())
except Exception as e:
    print("refresh raised", type(e).__name__)
red = displayio.Bitmap(d.width, d.height, 1)
p = displayio.Palette(1)
p[0] = 0xff0000
g2 = displayio.Group()
g2.append(displayio.TileGrid(red, pixel_shader=p))
d.root_group = g2
print("second", d.refresh())
print("drawn")
"""
with launch(CODE, files={"w.bmp": BMP}) as b:
    seen(b, "drawn")
    out = b.console(since=0)["data"]
    w, h, px = picture(b)
check("a closed file in a refresh does not raise", "refresh True" in out, out)
check("the display still draws after it", px(w // 2, h // 2) == (255, 0, 0), px(w // 2, h // 2))
print("DONE")
