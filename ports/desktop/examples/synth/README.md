# A synth on the MIDI keyboard

`code.py` is a `synthio` synth that reads USB MIDI with `adafruit_midi`, as it would on a board
with a MIDI keyboard plugged in: sawtooth notes through one low-pass filter, then an echo.
`board.toml` puts its controls on the board panel: a two-octave keyboard and three sliders that
send Control Changes.

From a release package:

    circuitpython --drive examples/synth/CIRCUITPY --board-file examples/synth/board.toml

From the port's directory after a build:

    build-linux/circuitpython --drive examples/synth/CIRCUITPY --board-file examples/synth/board.toml

- Click the keys, or play the computer's keyboard: the lower octave on `Z X C V B N M` (`Y X C`
  on a QWERTZ keyboard) with its black keys on `S D G H J`, the upper on `Q W E R T Y U` with its
  black keys on `2 3 5 6 7`. Page Up / Page Down move an octave.
- CUTOFF (CC 74) is the filter's corner frequency, 100 Hz to 8 kHz; RESO (CC 71) its resonance;
  ECHO (CC 91) how much echo is mixed in.

`lib/` has `adafruit_midi` from the bundle. `--trace-bus -` prints each MIDI message as it comes in.
