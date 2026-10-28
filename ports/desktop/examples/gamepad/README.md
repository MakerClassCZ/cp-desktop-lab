# A ship on the PicoPad's gamepad

`code.py` reads the PicoPad's buttons with `keypad.Keys`, as on the handheld: the D-pad flies a
ship over the stars, A fires, and the bottom line lists the buttons held. `board.toml` shows the
gamepad on the board panel.

From a release package:

    circuitpython --drive examples/gamepad/CIRCUITPY --board-file examples/gamepad/board.toml

From the port's directory after a build:

    build-linux/circuitpython --drive examples/gamepad/CIRCUITPY --board-file examples/gamepad/board.toml

- Click the panel's buttons, or use the keys: the arrows, A on `Z`, B on `X`, X on `A`, Y on `S`,
  START on Enter, SELECT on Backspace.
- A game controller plugged into the computer (Xbox, PlayStation, 8BitDo ...) presses the same
  buttons.

The same `code.py` runs on a PicoPad. `lib/` has `adafruit_display_text` from the bundle.
