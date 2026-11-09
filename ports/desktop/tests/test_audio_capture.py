"""Recording what the audio outputs play: python3 test_audio_capture.py /path/to/circuitpython

With --audio-capture (cpapi: audio_capture=True) the program keeps what AudioOut, I2SOut and
PWMAudioOut play, as /api/audio lists it and /api/audio/wav gives it; cpapi's Recording measures
it. audiocore.get_buffer() reads a sample's buffers from Python, as the unix coverage build does.
"""

from harness import check, launch

TONE = """import array, audiocore, audioio, board, math, time
rate = 22050
wave = array.array("h", [int(math.sin(2 * math.pi * i / 50) * 16000) for i in range(50)])
sample = audiocore.RawSample(wave, sample_rate=rate)
out = audioio.AudioOut(board.SPEAKER)
out.play(sample, loop=True)
time.sleep(0.5)
out.stop()
print("played")
"""

for fast in (False, True):
    how = "fast time" if fast else "real time"
    with launch(TONE, audio_capture=True, fast_time=fast) as b:
        b.wait("played", timeout=15)
        recs = b.audio()
        r = recs[0] if recs else None
        check("%s: one recording of the output" % how, len(recs) == 1, recs)
        if r is not None:
            check("%s: its rate and channels" % how, (r.rate, r.channels) == (22050, 1), (r.rate, r.channels))
            f = r.frequency()
            check("%s: the tone's frequency, 441 Hz" % how, f is not None and abs(f - 441) < 5, f)
            check("%s: about the half second played" % how, 0.4 < r.seconds < 0.75, r.seconds)
            check("%s: its level" % how, 0.45 < r.peak() < 0.5 and 0.3 < r.rms() < 0.37, (r.peak(), r.rms()))

with launch(TONE) as b:
    b.wait("played", timeout=15)
    check("without --audio-capture nothing is recorded", b.audio() == [])

GAP = """import array, audiocore, audioio, board, math, time
wave = array.array("h", [int(math.sin(2 * math.pi * i / 50) * 16000) for i in range(50)])
silence = array.array("h", [0] * 2205)
out = audioio.AudioOut(board.SPEAKER)
out.play(audiocore.RawSample(wave, sample_rate=22050), loop=True)
time.sleep(0.3)
out.play(audiocore.RawSample(silence, sample_rate=22050))
while out.playing:
    pass
out.play(audiocore.RawSample(wave, sample_rate=22050), loop=True)
time.sleep(0.3)
out.stop()
print("played")
"""

with launch(GAP, audio_capture=True, fast_time=True) as b:
    b.wait("played", timeout=15)
    r = b.audio()[0]
    gaps = r.silences(min_ms=50)
    check("a 100 ms silence between two tones is found", len(gaps) == 1 and 0.08 < gaps[0][1] - gaps[0][0] < 0.12, gaps)

DEBUG = """import array, audiocore
s = audiocore.RawSample(array.array("h", [1, 2, 3, 4]), sample_rate=8000)
audiocore.reset_buffer(s)
result, buf = audiocore.get_buffer(s)
print("buffer", list(buf))
"""

with launch(DEBUG) as b:
    out = b.wait("buffer", timeout=10)
    out += b.wait("\n", timeout=5)
    check("audiocore.get_buffer() reads a sample's buffer", "buffer [1, 2, 3, 4]" in out, out)
print("DONE")
