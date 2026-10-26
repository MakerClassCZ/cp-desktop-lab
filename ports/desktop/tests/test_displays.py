"""Displays on the virtual buses: python3 test_displays.py /path/to/circuitpython

A board file's [spi] puts a colour TFT on the virtual SPI bus (fourwire, as adafruit_st7789
drives it), its [i2c] an SSD1306 (i2cdisplaybus, as adafruit_displayio_ssd1306 drives it). The
test reads what each display shows, upright, through the control API; the key script's
"dump NAME PATH" saves it as a PPM.
"""

import os, time
from harness import HEADLESS, T, check, launch, ppm, shown


# A 240x240 ST7789 on SPI, as adafruit_st7789's 240x240 example sets it up
TFT = """import board, displayio, fourwire, busdisplay, time
displayio.release_displays()
bus = fourwire.FourWire(board.SPI(), command=board.D9, chip_select=board.D10)
INIT = (b"\\x01\\x80\\x96" b"\\x11\\x80\\xff" b"\\x3a\\x81\\x55\\x0a" b"\\x36\\x01\\x08" b"\\x13\\x80\\x0a"
        b"\\x29\\x80\\xff" b"\\x36\\x01\\xc0" b"\\x21\\x00")
display = busdisplay.BusDisplay(bus, INIT, width=240, height=240, rowstart=80)
group = displayio.Group()
background = displayio.Bitmap(240, 240, 1)
palette = displayio.Palette(2)
palette[0] = 0x0000FF
palette[1] = 0xFF0000
group.append(displayio.TileGrid(background, pixel_shader=palette))
square = displayio.Bitmap(20, 20, 2)
square.fill(1)
group.append(displayio.TileGrid(square, pixel_shader=palette))
display.root_group = group
display.refresh()
print("drawn")
while True:
    time.sleep(1)
"""
TFT_BOARD = '[pins]\nD9 = "GPIO18"\nD10 = "GPIO19"\n[spi]\nTFT = "st7789 240x240 cs=D10 dc=D9"\n'
out, (img,) = shown(TFT, TFT_BOARD, ["TFT"])
check("ST7789: the program draws", "drawn" in out, out)
check("ST7789: the picture is 240x240", img is not None and img[:2] == (240, 240), img and img[:2])
if img:
    w, h, px = img
    check(
        "ST7789: the red square at the top left, upright",
        px(5, 5)[0] > 200 and px(5, 5)[2] < 50,
        px(5, 5),
    )
    check(
        "ST7789: blue elsewhere, the far corner too",
        px(120, 120)[2] > 200 and px(235, 235)[2] > 200,
        (px(120, 120), px(235, 235)),
    )

# The key script's dump: the same picture as a PPM
TFT_PPM = os.path.join(T, "disp_tft.ppm")
KEYS = os.path.join(T, "disp_keys.txt")
for path in (TFT_PPM, KEYS):
    if os.path.exists(path):
        os.remove(path)
open(KEYS, "w").write("sleep 2000\ndump TFT %s\n" % TFT_PPM)
with launch(TFT, TFT_BOARD, env=dict(HEADLESS, CIRCUITPY_SDL_KEYS=KEYS)) as b:
    end = time.time() + 10
    while time.time() < end and not any("dumped TFT" in line for line in b.stderr):
        time.sleep(0.2)
dump = ppm(TFT_PPM)
check(
    "the key script dumps a display as a PPM",
    dump is not None and dump[:2] == (240, 240),
    dump and dump[:2],
)
os.remove(KEYS)

# A 128x64 SSD1306 on I2C, as adafruit_displayio_ssd1306 sets it up
out, (img,) = shown(
    """import board, displayio, i2cdisplaybus, busdisplay, time
displayio.release_displays()
bus = i2cdisplaybus.I2CDisplayBus(board.I2C(), device_address=0x3C)
INIT = bytearray(b"\\xae\\x00\\x20\\x01\\x00\\x81\\x01\\xcf\\xa1\\x00\\xa6\\x00\\xc8\\x00\\xa8\\x01\\x3f"
                 b"\\xd5\\x01\\x80\\xd9\\x01\\xf1\\xda\\x01\\x12\\xdb\\x01\\x40\\x8d\\x01\\x14\\xaf\\x00")
display = busdisplay.BusDisplay(bus, INIT, width=128, height=64, colstart=0, rowstart=0, color_depth=1,
    grayscale=True, pixels_in_byte_share_row=False, set_column_command=0x21, set_row_command=0x22,
    data_as_commands=True, brightness_command=0x81, single_byte_bounds=True)
group = displayio.Group()
square = displayio.Bitmap(10, 10, 1)
palette = displayio.Palette(2)
palette[0] = 0x000000
palette[1] = 0xFFFFFF
square.fill(0)
for i in range(10):
    for j in range(10):
        square[i, j] = 1
group.append(displayio.TileGrid(square, pixel_shader=palette, x=0, y=0))
display.root_group = group
display.refresh()
print("drawn")
while True:
    time.sleep(1)
""",
    '[i2c]\nOLED = "ssd1306 128x64"\n',
    ["OLED"],
)
check("SSD1306: the program draws", "drawn" in out, out)
check("SSD1306: the picture is 128x64", img is not None and img[:2] == (128, 64), img and img[:2])
if img:
    w, h, px = img
    check("SSD1306: the lit square at the top left, upright", px(5, 5)[0] > 200, px(5, 5))
    check(
        "SSD1306: dark elsewhere, the far corner too",
        px(64, 32)[0] < 50 and px(125, 60)[0] < 50,
        (px(64, 32), px(125, 60)),
    )

# Displays no board file names: a fourwire display on any cs pin gets a TFT as large as the
# program says, turned back by its rotation; an I2C display bus an SSD1306 at its address.
out, (img, oled) = shown(
    """import board, busio, displayio, fourwire, busdisplay, i2cdisplaybus, time
displayio.release_displays()
spi = busio.SPI(board.SCK, MOSI=board.MOSI)
bus = fourwire.FourWire(spi, command=board.D9, chip_select=board.D10)
INIT = (b"\\x01\\x80\\x96" b"\\x11\\x80\\xff" b"\\x3a\\x81\\x55\\x0a" b"\\x36\\x01\\x08" b"\\x13\\x80\\x0a" b"\\x29\\x80\\xff")
tft = busdisplay.BusDisplay(bus, INIT, width=240, height=135, rowstart=40, colstart=53, rotation=270)
oled_bus = i2cdisplaybus.I2CDisplayBus(board.I2C(), device_address=0x3d)
OLED_INIT = (b"\\xae\\x00" b"\\x20\\x01\\x00" b"\\x40\\x00" b"\\xa8\\x01\\x1f" b"\\xaf\\x00")
oled = busdisplay.BusDisplay(oled_bus, OLED_INIT, width=128, height=32, color_depth=1, grayscale=True,
    pixels_in_byte_share_row=False, set_column_command=0x21, set_row_command=0x22, data_as_commands=True,
    brightness_command=0x81, single_byte_bounds=True)
palette = displayio.Palette(2)
palette[0] = 0x0000FF
palette[1] = 0xFF0000
g = displayio.Group()
g.append(displayio.TileGrid(displayio.Bitmap(240, 135, 1), pixel_shader=palette))
sq = displayio.Bitmap(30, 20, 2)
sq.fill(1)
g.append(displayio.TileGrid(sq, pixel_shader=palette))
tft.root_group = g
tft.refresh()
print("drawn")
while True:
    time.sleep(1)
""",
    '[pins]\nD9 = "GPIO18"\nD10 = "GPIO19"\n',
    ["TFT", "OLED"],
)
check(
    "a fourwire display no board file names gets a TFT",
    "drawn" in out and img is not None and img[:2] == (240, 135),
    (out, img and img[:2]),
)
if img:
    w, h, px = img
    check(
        "the TFT shows the program's picture upright at rotation 270",
        px(5, 5)[0] > 200 and px(200, 100)[2] > 200,
        (px(5, 5), px(200, 100)),
    )
check(
    "an I2C display bus at 0x3d finds an SSD1306 there, as large as the driver says",
    oled[:2] == (128, 32),
    (out, oled[:2]),
)
print("DONE")
