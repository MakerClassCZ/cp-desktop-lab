# The PicoPad's buttons through keypad, as on the handheld: the D-pad moves a ship over the stars,
# A fires, and the buttons held are listed.
import random
import time
import board
import displayio
import keypad
import terminalio
import vectorio
from adafruit_display_text import label

NAMES = ("UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "START", "SELECT")
keys = keypad.Keys([getattr(board, "SW_" + n) for n in NAMES], value_when_pressed=False, pull=True)
held = set()

W, H = board.DISPLAY.width, board.DISPLAY.height
g = displayio.Group()
white = displayio.Palette(1)
white[0] = 0x8090B0
stars = [
    vectorio.Rectangle(
        pixel_shader=white, width=2, height=2, x=random.randrange(W), y=random.randrange(H)
    )
    for _ in range(60)
]
for s in stars:
    g.append(s)
ship_colour = displayio.Palette(1)
ship_colour[0] = 0x40D0FF
ship = vectorio.Polygon(
    pixel_shader=ship_colour, points=[(0, 0), (28, 10), (0, 20), (6, 10)], x=60, y=110
)
g.append(ship)
shot_colour = displayio.Palette(1)
shot_colour[0] = 0xFFD000
shots = []
g.append(label.Label(terminalio.FONT, text="PicoPad gamepad", color=0xFFD000, scale=2, x=10, y=16))
status = label.Label(terminalio.FONT, text="", color=0xFFFFFF, x=10, y=H - 12)
g.append(status)
board.DISPLAY.root_group = g

last_shot = 0
while True:
    while event := keys.events.get():
        name = NAMES[event.key_number]
        (held.add if event.pressed else held.discard)(name)
    ship.x = max(0, min(W - 28, ship.x + 4 * (("RIGHT" in held) - ("LEFT" in held))))
    ship.y = max(30, min(H - 40, ship.y + 4 * (("DOWN" in held) - ("UP" in held))))
    if "A" in held and time.monotonic() - last_shot > 0.15 and len(shots) < 8:
        shot = vectorio.Rectangle(
            pixel_shader=shot_colour, width=10, height=3, x=ship.x + 28, y=ship.y + 9
        )
        g.append(shot)
        shots.append(shot)
        last_shot = time.monotonic()
    for shot in shots[:]:
        shot.x += 9
        if shot.x > W:
            g.remove(shot)
            shots.remove(shot)
    for s in stars:
        s.x = (s.x - 1) % W
    status.text = "held: " + (" ".join(n for n in NAMES if n in held) or "-")
    time.sleep(0.03)
