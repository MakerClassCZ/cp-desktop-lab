# The board panel demo: every kind of part at once.
#   KNOB (a slider)   sets the lamp's brightness, the servo's angle, the METER bar and the strip
#   the microphone    drives the matrix as a level meter (move its slider), or a rainbow with SWITCH
#   BUTTON (or Z)     beeps through the speaker and lights the LED while held
#   PAD               flashes the strip white while touched
import array
import math
import time

import analogio
import audiobusio
import audiocore
import board
import digitalio
import neopixel_write
import pwmio
import touchio


def show(pin, pixels):
    """(r, g, b) pixels to a WS2812 strip, which takes them as G, R, B."""
    neopixel_write.neopixel_write(pin, bytes(c for r, g, b in pixels for c in (g, r, b)))


def wheel(pos):
    pos %= 255
    if pos < 85:
        return (255 - pos * 3, pos * 3, 0)
    if pos < 170:
        pos -= 85
        return (0, 255 - pos * 3, pos * 3)
    pos -= 170
    return (pos * 3, 0, 255 - pos * 3)


def output(pin):
    d = digitalio.DigitalInOut(pin)
    d.switch_to_output()
    return d


strip = output(board.NEOPIXEL)
matrix = output(board.MATRIX)
led = output(board.LED)
button = digitalio.DigitalInOut(board.BUTTON)  # reads low while pressed
switch = digitalio.DigitalInOut(board.SWITCH)
knob = analogio.AnalogIn(board.KNOB)
meter = analogio.AnalogOut(board.METER)
lamp = pwmio.PWMOut(board.LAMP, frequency=1000)
servo = pwmio.PWMOut(board.SERVO, frequency=50)
pad = touchio.TouchIn(board.PAD)
mic = audiobusio.PDMIn(board.MIC_CLK, board.MIC_DATA, sample_rate=16000, bit_depth=16)
speaker = audiobusio.I2SOut(board.I2S_BCLK, board.I2S_WS, board.I2S_DATA)
beep = audiocore.RawSample(
    array.array("h", [int(math.sin(i * math.pi / 8) * 8000) for i in range(16)]), sample_rate=8000
)
samples = array.array("H", [0] * 160)

print("board panel demo: move KNOB, the microphone's slider, SWITCH; hold BUTTON or PAD")
frame = 0
last_print = 0
while True:
    k = knob.value
    lamp.duty_cycle = k
    servo.duty_cycle = int(65535 * (1 + k / 65535) / 20)  # a 1-2 ms pulse every 20 ms
    meter.value = k

    lit = k * 9 // 65536
    pixels = [(0, 0, 0)] * 8 if not pad.value else [(255, 255, 255)] * 8
    if not pad.value:
        for i in range(lit):
            pixels[i] = wheel(i * 30)
    show(strip, pixels)

    mic.record(samples, len(samples))
    level = (max(samples) - min(samples)) * 8 // 65536
    grid = [(0, 0, 0)] * 64
    for y in range(8):
        for x in range(8):
            if switch.value:
                colour = wheel((x + y) * 16 + frame * 4)
            else:
                colour = wheel(80 - y * 10) if 7 - y < level else (0, 0, 0)
            # the matrix is wired serpentine: every other row runs backwards
            grid[y * 8 + (7 - x if y % 2 else x)] = colour
    show(matrix, grid)

    pressed = not button.value
    if pressed and not speaker.playing:
        speaker.play(beep, loop=True)
    elif not pressed and speaker.playing:
        speaker.stop()
    led.value = pressed or (frame // 10) % 2 == 0

    now = time.monotonic()
    if now - last_print >= 1:
        last_print = now
        print(
            "knob %5d  mic level %d/8  switch %s  pad %s  button %s"
            % (k, level, switch.value, pad.value, pressed)
        )
    frame += 1
