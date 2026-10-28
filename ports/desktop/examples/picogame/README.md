# The gamepad example in picogame

`examples/gamepad` again, drawn with `picogame`, CircuitPython's game engine: the D-pad flies a
ship over the stars with a flame behind it (`picogame.Particles`), A fires, and a shot that hits
a rock breaks it into sparks. The ship and the rocks are bitmaps the program computes; the text
is `terminalio.FONT` drawn into a `picogame.Canvas`. `board.toml` shows the gamepad on the board
panel.

From a release package:

    circuitpython --drive examples/picogame/CIRCUITPY --board-file examples/picogame/board.toml

From the port's directory after a build:

    build-linux/circuitpython --drive examples/picogame/CIRCUITPY --board-file examples/picogame/board.toml

- Click the panel's buttons, or use the keys: the arrows, A on `Z`, B on `X`, X on `A`, Y on `S`,
  START on Enter, SELECT on Backspace.
- A game controller plugged into the computer (Xbox, PlayStation, 8BitDo ...) presses the same
  buttons.

`lib/` has `picogame_game` from the [picogame helper libraries](https://github.com/MakerClassCZ/picogame-libs):
its `setup()` takes over the display, the window's framebuffer here and the SPI display on a
PicoPad, so the same `code.py` runs on both.
