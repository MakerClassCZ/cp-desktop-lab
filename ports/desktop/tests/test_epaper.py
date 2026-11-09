"""The e-paper model: python3 test_epaper.py /path/to/circuitpython

An EPaperDisplay on a fourwire bus of the virtual board gets a panel (EPD) that takes the
driver's commands as its controller would: the black and colour RAM written through windows,
shown only at a refresh, after which the busy pin is held for the refresh time. A board file's
"epaper cs=PIN refresh_ms=N stuck" sets that time or keeps the panel busy.
"""

from harness import check, launch

COMMON = """import board, displayio, epaperdisplay, fourwire, time
displayio.release_displays()
bus = fourwire.FourWire(board.SPI(), command=board.GPIO9, chip_select=board.GPIO8, baudrate=1000000)
"""

# The SSD1680 driver's arguments, as on a MagTag.
SSD1680 = (
    COMMON
    + """start = (b"\\x12\\x80\\x00\\x14" b"\\x11\\x00\\x01\\x03" b"\\x4e\\x00\\x01\\x01" b"\\x4f\\x00\\x02\\x00\\x00"
    b"\\x01\\x00\\x03\\x27\\x01\\x00")
d = epaperdisplay.EPaperDisplay(bus, start, b"\\x10\\x80\\x01\\x01\\x64", width=296, height=128,
    rotation=ROTATION, ram_width=250, ram_height=296, busy_pin=board.GPIO10, busy_state=True,
    write_black_ram_command=0x24, write_color_ram_command=0x26, set_column_window_command=0x44,
    set_row_window_command=0x45, set_current_column_command=0x4E, set_current_row_command=0x4F,
    refresh_display_command=0x20, address_little_endian=True, two_byte_sequence_length=True,
    highlight_color=HIGHLIGHT)
"""
)

# The SPD1656 driver's (7-colour ACeP, 5.65").
ACEP = (
    COMMON
    + """start = b"\\x00\\x02\\xef\\x08" b"\\x61\\x04\\x02\\x58\\x01\\xc0" b"\\x04\\x80\\xc8"
d = epaperdisplay.EPaperDisplay(bus, start, b"\\x02\\x01\\x00\\x07\\x01\\xa5", width=600, height=448,
    ram_width=640, ram_height=480, start_up_time=1, busy_pin=board.GPIO10, busy_state=False,
    write_black_ram_command=0x10, refresh_display_command=0x12, advanced_color_epaper=True)
"""
)

# White, the top left quarter black, the bottom right one red; refreshed, then waited out.
DRAW = """g = displayio.Group()
bmp = displayio.Bitmap(d.width, d.height, 3)
pal = displayio.Palette(3)
pal[0], pal[1], pal[2] = 0xFFFFFF, 0x000000, 0xFF0000
bmp.fill(0)
for y in range(d.height // 2):
    for x in range(d.width // 2):
        bmp[x, y] = 1
for y in range(d.height // 2, d.height):
    for x in range(d.width // 2, d.width):
        bmp[x, y] = 2
g.append(displayio.TileGrid(bmp, pixel_shader=pal))
d.root_group = g
d.refresh()
print("busy after refresh", d.busy)
t = time.monotonic()
while d.busy and time.monotonic() - t < 3:
    time.sleep(0.01)
print("refreshed, busy", d.busy)
"""


def quarters(b):
    p = b.pixels("EPD")
    w, h = p.width, p.height
    return [
        p[w // 4, h // 4],
        p[3 * w // 4, h // 4],
        p[w // 4, 3 * h // 4],
        p[3 * w // 4, 3 * h // 4],
    ]


def epd(b):
    return next(x for x in b.state()["displays"] if x["name"] == "EPD")


WHITE, BLACK, RED = 0xFFFFFF, 0x000000, 0xFF0000

for rotation, highlight, want in (
    (270, 0, [BLACK, WHITE, WHITE, BLACK]),
    (270, 0xFF0000, [BLACK, WHITE, WHITE, RED]),
    (90, 0xFF0000, [BLACK, WHITE, WHITE, RED]),
):
    code = SSD1680.replace("ROTATION", str(rotation)).replace("HIGHLIGHT", hex(highlight)) + DRAW
    name = "SSD1680, rotation %d%s" % (rotation, ", red" if highlight else "")
    with launch(code) as b:
        out = b.wait("refreshed", timeout=20)
        out += b.wait("\n", timeout=5)
        check(name + ": the picture, upright", quarters(b) == want, [hex(c) for c in quarters(b)])
        check(
            name + ": busy after the refresh, then not",
            "busy after refresh True" in out and "refreshed, busy False" in out,
            out[-200:],
        )
        check(name + ": one refresh counted", epd(b)["refreshes"] == 1, epd(b))
        check(name + ": the trace has it", "EPD EPD refresh 1" in b.trace(since=0)["data"])

STUCK = '[spi]\nEPD = "epaper cs=GPIO8 stuck"\n'
with launch(SSD1680.replace("ROTATION", "270").replace("HIGHLIGHT", "0") + DRAW, board=STUCK) as b:
    out = b.wait("refreshed", timeout=20)
    out += b.wait("\n", timeout=5)
    check("stuck: busy for ever", "refreshed, busy True" in out and epd(b)["busy"], out[-200:])

SLOW = '[spi]\nEPD = "epaper cs=GPIO8 refresh_ms=1500"\n'
TIMED = DRAW.replace(
    'print("refreshed, busy", d.busy)',
    'print("refreshed, busy", d.busy, "after", round(time.monotonic() - t, 1))',
)
with launch(SSD1680.replace("ROTATION", "270").replace("HIGHLIGHT", "0") + TIMED, board=SLOW) as b:
    out = b.wait("refreshed", timeout=20)
    out += b.wait("\n", timeout=5)
    check(
        "refresh_ms: busy that long",
        "busy False after 1.5" in out or "busy False after 1.6" in out,
        out[-120:],
    )

with launch(ACEP + DRAW) as b:
    try:
        out = b.wait("refreshed", timeout=20)
        check(
            "ACeP: the picture in its colours",
            quarters(b) == [BLACK, WHITE, WHITE, RED],
            [hex(c) for c in quarters(b)],
        )
        check("ACeP: the clean and the picture, two refreshes", epd(b)["refreshes"] == 2, epd(b))
    except TimeoutError:
        # _clean_area() returns with the bus locked where adafruit/circuitpython#11808 is not in.
        print("SKIP ACeP: refresh() hangs in _clean_area (adafruit/circuitpython#11808)")
print("DONE")
