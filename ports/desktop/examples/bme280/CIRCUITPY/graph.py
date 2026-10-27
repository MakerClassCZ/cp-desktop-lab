# The same sensor drawn on the display: the temperature in large digits and its last minute as
# bars, blue when cold and red when hot. Copy it over code.py to run it.
import time

import board
import displayio
import terminalio
import vectorio
from adafruit_display_text import label

from adafruit_bme280 import basic as adafruit_bme280

bme280 = adafruit_bme280.Adafruit_BME280_I2C(board.I2C())

display = board.DISPLAY
W, H = display.width, display.height
BARS = 60
LOW, HIGH = 0.0, 40.0  # the chart's range, in degrees C
TOP, BOTTOM = 70, H - 16  # the chart's area

screen = displayio.Group()
display.root_group = screen

reading = label.Label(terminalio.FONT, text="", color=0xFFFFFF, scale=4, x=10, y=28)
detail = label.Label(terminalio.FONT, text="", color=0x909090, x=12, y=56)
screen.append(reading)
screen.append(detail)
screen.append(label.Label(terminalio.FONT, text="%d C" % HIGH, color=0x606060, x=W - 30, y=TOP))
screen.append(label.Label(terminalio.FONT, text="%d C" % LOW, color=0x606060, x=W - 30, y=BOTTOM))


def color_of(t):
    """Blue at LOW, red at HIGH."""
    f = min(max((t - LOW) / (HIGH - LOW), 0.0), 1.0)
    return (int(255 * f) << 16) | (40 << 8) | int(255 * (1 - f))


step = (W - 40) // BARS
bars = []
for i in range(BARS):
    palette = displayio.Palette(1)
    palette[0] = 0x202020
    bar = vectorio.Rectangle(
        pixel_shader=palette, width=step - 1, height=1, x=4 + i * step, y=BOTTOM
    )
    screen.append(bar)
    bars.append((bar, palette))

history = []
while True:
    t = bme280.temperature
    history = (history + [t])[-BARS:]
    reading.text = "%.1f C" % t
    detail.text = "%.0f %% humidity   %.0f hPa" % (bme280.relative_humidity, bme280.pressure)
    for (bar, palette), value in zip(bars[BARS - len(history) :], history):
        f = min(max((value - LOW) / (HIGH - LOW), 0.0), 1.0)
        height = max(1, int(f * (BOTTOM - TOP)))
        bar.height = height
        bar.y = BOTTOM - height
        palette[0] = color_of(value)
    time.sleep(1)
