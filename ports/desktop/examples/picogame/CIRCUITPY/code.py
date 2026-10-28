# The gamepad example in picogame: the D-pad flies a ship over the stars with a flame behind it,
# A fires, and a shot that hits a rock breaks it into sparks.
import array
import random
import time

import board
import keypad
import picogame as pg
import picogame_game
import terminalio

NAMES = ("UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "START", "SELECT")
keys = keypad.Keys([getattr(board, "SW_" + n) for n in NAMES], value_when_pressed=False, pull=True)
held = set()

BLACK = pg.rgb565(0, 0, 0)
scene, _, _ = picogame_game.setup(background=BLACK)
W, H = board.DISPLAY.width, board.DISPLAY.height


def shape(w, h, colours, inside):
    """A PAL8 bitmap: inside(x, y) gives each pixel's colour index, 0 is transparent."""
    palette = array.array("H", [BLACK] + [pg.rgb565(*c) for c in colours])
    data = bytearray(inside(x, y) for y in range(h) for x in range(w))
    return pg.Bitmap(data, w, h, format=pg.PAL8, palette=palette, transparent=0)


def ship_pixel(x, y):
    # An arrowhead: the outline (0, 0) (28, 10) (0, 20) with a notch at the back to (6, 10).
    d = abs(y - 10)
    if x > 28 - d * 2.8 or x < 6 - d * 0.6:
        return 0
    return 2 if 14 <= x <= 20 and d < 2 else 1


def rock_pixel(x, y):
    d = (x - 9) ** 2 + (y - 9) ** 2
    return 0 if d > 81 else (2 if (x * 7 + y * 3) % 11 == 0 else 1)


star_bitmap = shape(2, 2, [(128, 144, 176)], lambda x, y: 1)
stars = [
    scene.add(pg.Sprite(star_bitmap, random.randrange(W), random.randrange(H))) for _ in range(60)
]

flame = scene.add(pg.Particles(160, size=2, fade=True))
sparks = scene.add(pg.Particles(160, size=2, fade=True))

ship = scene.add(pg.Sprite(shape(29, 21, [(64, 208, 255), (255, 255, 255)], ship_pixel), 60, 110))

rock_bitmap = shape(19, 19, [(150, 130, 110), (90, 75, 60)], rock_pixel)
rocks = [
    scene.add(pg.Sprite(rock_bitmap, W + i * 110, random.randrange(30, H - 50))) for i in range(3)
]

shot_bitmap = shape(10, 3, [(255, 208, 0)], lambda x, y: 1)
shots = [scene.add(pg.Sprite(shot_bitmap, visible=False)) for _ in range(8)]

title = scene.add(pg.Canvas(W, 16, transparent=BLACK))
title.clear(BLACK)
title.text(10, 4, "picogame gamepad", pg.rgb565(255, 208, 0), terminalio.FONT)
status = scene.add(pg.Canvas(W, 12, transparent=BLACK))
status.move(0, H - 14)

FLAME = pg.rgb565(255, 140, 20)
SPARK = pg.rgb565(255, 220, 120)
last_shot = 0
shown = None
while True:
    while event := keys.events.get():
        name = NAMES[event.key_number]
        (held.add if event.pressed else held.discard)(name)
    dx = ("RIGHT" in held) - ("LEFT" in held)
    dy = ("DOWN" in held) - ("UP" in held)
    ship.move(max(0, min(W - 29, ship.x + 4 * dx)), max(20, min(H - 40, ship.y + 4 * dy)))
    flame.emit(ship.x + 2, ship.y + 10, 5, 1, 14, FLAME)

    now = time.monotonic()
    if "A" in held and now - last_shot > 0.15:
        for shot in shots:
            if not shot.visible:
                shot.move(ship.x + 28, ship.y + 9)
                shot.visible = True
                last_shot = now
                break
    for shot in shots:
        if shot.visible:
            shot.x += 9
            if shot.x > W:
                shot.visible = False

    for rock in rocks:
        rock.x -= 2
        hit = False
        for shot in shots:
            if shot.visible and shot.overlaps(rock):
                shot.visible = False
                sparks.emit(rock.x + 9, rock.y + 9, 40, 3, 25, SPARK)
                hit = True
                break
        if hit or rock.x < -19:
            rock.move(W + random.randrange(40), random.randrange(30, H - 50))

    for star in stars:
        star.x = (star.x - 1) % W
    flame.tick()
    sparks.tick()

    text = "held: " + (" ".join(n for n in NAMES if n in held) or "-")
    if text != shown:
        status.clear(BLACK)
        status.text(10, 2, text, pg.rgb565(255, 255, 255), terminalio.FONT)
        shown = text
    scene.refresh()
    time.sleep(0.02)
