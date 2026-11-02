# Desktop port

CircuitPython as a program on a computer: a virtual board with its panel, or real pins through a
USB adapter, driven by a person, a test or an agent.

## The virtual board

**Part**:
A thing put on the virtual board for a program to use: a button, a LED, a slider, an encoder, a
speaker, a sensor's value. Most sit on a pin; a sensor's value and a CC slider sit on none.
_Avoid_: device, widget, component

**Part kind**:
What a part is. A kind is named in each place a person meets it, and the names differ on purpose:
the program's (`Potentiometer`, `Lamp`, `Servo`), the board file's section (`[analog]`, `[pwm]`)
and the panel's state (`analog`, `pwm`).
_Avoid_: part type

## The board panel

**Panel**:
The window of the virtual board that shows its parts, NeoPixels, displays, gamepad and MIDI
keyboard, and lets a person work them.
_Avoid_: GUI, board window

**Panel line**:
One command of the panel's language (set, press, release, tap, turn, send, down, up, dump, close),
the same whether a person's tool, a test or an agent sends it.
_Avoid_: panel command, key command

**Key script**:
A file of panel lines, with pauses between them, played into a running program from its start.
_Avoid_: keys file, macro
