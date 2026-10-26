# The board panel demo

Every kind of part on the board panel at once, on the virtual board. From a release package:

    circuitpython --drive examples/panel/CIRCUITPY --board-file examples/panel/board.toml

From the port's directory after a build:

    build-windows/circuitpython.exe --drive examples/panel/CIRCUITPY --board-file examples/panel/board.toml
    build-linux/circuitpython --drive examples/panel/CIRCUITPY --board-file examples/panel/board.toml

- KNOB (a slider: drag it, or the wheel over it, Shift for one count) sets the strip, the lamp's
  brightness, the servo's angle and the METER bar.
- The microphone's slider is the level the 8x8 matrix shows; SWITCH turns the matrix into a
  rainbow. With `MIC_DATA = "mic"` in `board.toml` the matrix listens to the computer's microphone.
- BUTTON (or the Z key) beeps through the speakers and lights the LED while held.
- PAD flashes the strip white while touched.

`--trace-bus -` prints every change of every part to the terminal as it happens.
