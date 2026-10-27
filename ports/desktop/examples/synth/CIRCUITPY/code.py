# A synthio synth played over USB MIDI, as on a board: notes from a keyboard, a low-pass filter's
# cutoff (CC 74) and resonance (CC 71), and an echo's mix (CC 91). On the virtual board the
# keyboard and the three sliders are on the board panel (board.toml).
import array

import audiodelays
import audioio
import board
import synthio
import usb_midi

import adafruit_midi
from adafruit_midi.control_change import ControlChange
from adafruit_midi.note_off import NoteOff
from adafruit_midi.note_on import NoteOn

RATE = 22050

audio = audioio.AudioOut(board.SPEAKER)
echo = audiodelays.Echo(
    max_delay_ms=600, delay_ms=300, decay=0.5, mix=0.0, buffer_size=1024, sample_rate=RATE
)
synth = synthio.Synthesizer(sample_rate=RATE)
audio.play(echo)
echo.play(synth)

# One filter for every note, so a slider changes the notes already sounding too.
lowpass = synthio.Biquad(synthio.FilterMode.LOW_PASS, frequency=2000, Q=1.0)
envelope = synthio.Envelope(attack_time=0.01, decay_time=0.2, sustain_level=0.7, release_time=0.4)
saw = array.array("h", [int(-32000 + 64000 * i / 255) for i in range(256)])

midi = adafruit_midi.MIDI(midi_in=usb_midi.ports[0], in_channel=0)
playing = {}

print(
    "Play the keyboard (the panel's, or Z S X D ... and Q 2 W 3 ...), move CUTOFF, RESO and ECHO."
)
while True:
    msg = midi.receive()
    if isinstance(msg, NoteOn) and msg.velocity > 0:
        note = synthio.Note(
            synthio.midi_to_hz(msg.note),
            waveform=saw,
            envelope=envelope,
            filter=lowpass,
            amplitude=msg.velocity / 127,
        )
        playing[msg.note] = note
        synth.press(note)
    elif isinstance(msg, NoteOff) or (isinstance(msg, NoteOn) and msg.velocity == 0):
        note = playing.pop(msg.note, None)
        if note is not None:
            synth.release(note)
    elif isinstance(msg, ControlChange):
        if msg.control == 74:  # cutoff, 100 Hz to 8 kHz
            lowpass.frequency = 100 * 80 ** (msg.value / 127)
        elif msg.control == 71:  # resonance
            lowpass.Q = 0.7 + 7.3 * msg.value / 127
        elif msg.control == 91:  # echo
            echo.mix = msg.value / 127
        print("cc", msg.control, msg.value)
