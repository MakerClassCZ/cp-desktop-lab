# The gamepad example in picogame: the D-pad flies a ship over the stars with a flame behind it,
# A fires, and a shot that hits a rock breaks it into sparks. A demo of the buttons, not a game:
# no lives, no title and no game over.
import board
import random
import terminalio
import picogame as pg
import picogame_game
import picogame_input
import picogame_clock
import picogame_pool
import picogame_sfx
import picogame_shapes as shapes
import picogame_synth
import picogame_ui as ui

# Tuning: the game's source is on the drive, so these are its settings.
SHIP_SPEED = 5
SHOT_SPEED = 10
FIRE_EVERY = 5  # frames between shots while A is held
ROCK_SPEED = 2
STARS = 30

BG = pg.rgb565(0, 0, 0)
INK = pg.rgb565(255, 255, 255)
GOLD = pg.rgb565(255, 208, 0)
BAR = 14  # the HUD bars at the top and the bottom

W, H = picogame_game.screen()
scene, bufA, bufB = picogame_game.setup(background=BG, top=BAR, bottom=BAR)
buttons = picogame_input.Buttons()  # the PicoPad's buttons, and a USB pad where there is one
clock = picogame_clock.Clock(30)
sfx = picogame_sfx.Kit(picogame_synth.Synth())  # silent where the board has no audio

top_bar = ui.HudBar(pg, board.DISPLAY, bufA, 0, 0, W, BAR, BG)
top_bar.label(terminalio.FONT, 4, 2, GOLD, "picogame gamepad")
score_label = top_bar.label(terminalio.FONT, W - 70, 2, INK, "ROCKS 0")
bottom_bar = ui.HudBar(pg, board.DISPLAY, bufA, 0, H - BAR, W, BAR, BG)
held_label = bottom_bar.label(terminalio.FONT, 4, H - BAR + 2, INK, "held: -")

# Generated shapes: stars in two speeds for depth, the ship, the rocks and the shots.
star_bitmap = shapes.rect(2, 2, pg.rgb565(128, 144, 176))
stars = []
for i in range(STARS):
    star = scene.add(pg.Sprite(star_bitmap, random.randrange(W), random.randrange(BAR, H - BAR)))
    star.data = 1 + i % 2  # its speed
    stars.append(star)

flame = scene.add(pg.Particles(160, size=2, fade=True))
sparks = scene.add(pg.Particles(160, size=2, fade=True))
FLAME = pg.rgb565(255, 140, 20)
SPARK = pg.rgb565(255, 220, 120)

SHIP = 30
ship_points = [(14, 0), (-14, -10), (-8, 0), (-14, 10)]  # an arrowhead pointing right
ship_bitmap = shapes.poly_frames(SHIP, ship_points, 1, pg.rgb565(64, 208, 255))
ship = scene.add(pg.Sprite(ship_bitmap, 60, 110))

ROCK = 18
rock_bitmap = shapes.circle(ROCK, pg.rgb565(150, 130, 110))
rocks = [
    scene.add(pg.Sprite(rock_bitmap, W + i * 110, random.randrange(BAR, H - BAR - ROCK)))
    for i in range(3)
]

# Pool size: a shot every FIRE_EVERY frames, each in flight W / SHOT_SPEED frames, plus spares.
shots = picogame_pool.Pool(scene, shapes.rect(10, 3, GOLD), W // SHOT_SPEED // FIRE_EVERY + 2)

NAMES = (
    ("UP", buttons.UP),
    ("DOWN", buttons.DOWN),
    ("LEFT", buttons.LEFT),
    ("RIGHT", buttons.RIGHT),
    ("A", buttons.A),
    ("B", buttons.B),
    ("X", buttons.X),
    ("Y", buttons.Y),
    ("START", buttons.START),
    ("SELECT", buttons.SELECT),
)


def held_text(mask):
    return "held: " + (" ".join(name for name, bit in NAMES if mask & bit) or "-")


def main():
    # The loop is a function: its names are fast locals, the hot calls bound once.
    poll = buttons.poll
    pressed = buttons.is_pressed
    refresh = scene.refresh
    tick = clock.tick
    UP, DOWN, LEFT, RIGHT, A = buttons.UP, buttons.DOWN, buttons.LEFT, buttons.RIGHT, buttons.A
    score = 0
    shown_mask = shown_score = -1
    reload = 0
    while True:
        mask = poll()
        x = ship.x + SHIP_SPEED * (pressed(RIGHT) - pressed(LEFT))
        y = ship.y + SHIP_SPEED * (pressed(DOWN) - pressed(UP))
        if x < 0:
            x = 0
        elif x > W - SHIP:
            x = W - SHIP
        if y < BAR:
            y = BAR
        elif y > H - BAR - SHIP:
            y = H - BAR - SHIP
        ship.move(x, y)
        flame.emit(x + 3, y + SHIP // 2, 5, 1, 14, FLAME)

        if reload:
            reload -= 1
        elif pressed(A):
            shot = shots.spawn()
            if shot:
                shot.move(x + SHIP - 4, y + SHIP // 2 - 1)
                sfx.zap()
                reload = FIRE_EVERY
        if shots.count():
            for shot in shots.items:
                if shot.visible:
                    shot.x += SHOT_SPEED
                    if shot.x > W:
                        shots.free(shot)

        for rock in rocks:
            rock.x -= ROCK_SPEED
            hit = False
            if shots.count():
                for shot in shots.items:
                    if shot.visible and shot.overlaps(rock):
                        shots.free(shot)
                        hit = True
                        break
            if hit:
                sparks.emit(rock.x + ROCK // 2, rock.y + ROCK // 2, 40, 3, 25, SPARK)
                sfx.boom()
                score += 1
            if hit or rock.x < -ROCK:
                rock.move(W + random.randrange(40), random.randrange(BAR, H - BAR - ROCK))

        for star in stars:
            star.x -= star.data
            if star.x < 0:
                star.x += W
        flame.tick()
        sparks.tick()
        sfx.tick()
        refresh()

        # The bars are outside the scene: drawn after refresh, and only when they change.
        if score != shown_score:
            shown_score = score
            score_label.set("ROCKS %d" % score)
            top_bar.draw()
        if mask != shown_mask:
            shown_mask = mask
            held_label.set(held_text(mask))
            bottom_bar.draw()
        tick()


main()
