# Four displays at once: the window, an ST7789 TFT (adafruit_st7789), an SSD1306 OLED
# (adafruit_displayio_ssd1306) and a 64x32 HUB75 matrix (rgbmatrix), all through displayio.
import math
import time
import board
import displayio
import fourwire
import i2cdisplaybus
import framebufferio
import rgbmatrix
import terminalio
import vectorio
import adafruit_st7789
import adafruit_displayio_ssd1306
from adafruit_display_text import label
from rainbowio import colorwheel

window = board.DISPLAY
tft = adafruit_st7789.ST7789(
    fourwire.FourWire(board.SPI(), command=board.TFT_DC, chip_select=board.TFT_CS),
    width=240,
    height=240,
    rowstart=80,
)
oled = adafruit_displayio_ssd1306.SSD1306(
    i2cdisplaybus.I2CDisplayBus(board.I2C(), device_address=0x3C), width=128, height=64
)
matrix = framebufferio.FramebufferDisplay(
    rgbmatrix.RGBMatrix(
        width=64,
        bit_depth=4,
        rgb_pins=[
            board.MTX_R1,
            board.MTX_G1,
            board.MTX_B1,
            board.MTX_R2,
            board.MTX_G2,
            board.MTX_B2,
        ],
        addr_pins=[board.MTX_ADDRA, board.MTX_ADDRB, board.MTX_ADDRC, board.MTX_ADDRD],
        clock_pin=board.MTX_CLK,
        latch_pin=board.MTX_LAT,
        output_enable_pin=board.MTX_OE,
    ),
    auto_refresh=True,
)

# The window: what is where.
g = displayio.Group()
g.append(label.Label(terminalio.FONT, text="Four displays", color=0xFFD000, scale=2, x=10, y=16))
g.append(
    label.Label(
        terminalio.FONT,
        color=0xFFFFFF,
        x=10,
        y=50,
        line_spacing=1.3,
        text="\n".join(
            (
                "board.DISPLAY  this window",
                "ST7789 TFT     on SPI, adafruit_st7789",
                "SSD1306 OLED   on I2C, displayio_ssd1306",
                "HUB75 matrix   rgbmatrix, 64x32",
                "",
                "Each on the board panel, as on",
                "the bench: same libraries, same code.",
            )
        ),
    )
)
window.root_group = g

# The TFT: circles in rainbow colours.
g = displayio.Group()
dots = []
for i in range(12):
    p = displayio.Palette(1)
    p[0] = colorwheel(i * 21)
    c = vectorio.Circle(pixel_shader=p, radius=16, x=120, y=120)
    g.append(c)
    dots.append(c)
g.append(label.Label(terminalio.FONT, text="ST7789", color=0xFFFFFF, scale=2, x=84, y=120))
tft.root_group = g

# The OLED: a frame and a counter.
g = displayio.Group()
white = displayio.Palette(1)
white[0] = 0xFFFFFF
g.append(vectorio.Rectangle(pixel_shader=white, width=128, height=2, x=0, y=0))
g.append(vectorio.Rectangle(pixel_shader=white, width=128, height=2, x=0, y=62))
g.append(label.Label(terminalio.FONT, text="SSD1306", color=0xFFFFFF, scale=2, x=22, y=18))
counter = label.Label(terminalio.FONT, text="", color=0xFFFFFF, x=30, y=46)
g.append(counter)
oled.root_group = g

# The matrix: scrolling text over a rainbow bar.
g = displayio.Group()
bars = []
for i in range(8):
    p = displayio.Palette(1)
    p[0] = colorwheel(i * 32)
    g.append(vectorio.Rectangle(pixel_shader=p, width=8, height=6, x=i * 8, y=26))
    bars.append(p)
text = label.Label(terminalio.FONT, text="CircuitPython", color=0xFF40C0, x=64, y=10)
g.append(text)
matrix.root_group = g

t = 0
while True:
    for i, c in enumerate(dots):
        a = t * 0.08 + i * math.pi / 6
        c.x = int(120 + 80 * math.cos(a))
        c.y = int(120 + 80 * math.sin(a))
    counter.text = "frame %d" % t
    text.x = 64 - (t % 140)
    for i, p in enumerate(bars):
        p[0] = colorwheel((i * 32 + t * 4) & 255)
    t += 1
    time.sleep(0.03)
