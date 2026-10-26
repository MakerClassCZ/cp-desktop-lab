"""The board panel: python3 test_panel.py /path/to/circuitpython

Virtual parts from a board file, driven headless through the control API's panel commands
("set NAME VALUE", "press NAME", "release NAME"), each after the program has said it is ready,
and checked in the program's output and the bus trace. One case plays the same commands from the
key script (CIRCUITPY_SDL_KEYS).
"""
import os, re
from harness import HEADLESS, check, launch, run, seen

BOARD_FILE = '''[pins]
D9 = "GPIO18"
D10 = "GPIO19"
D13 = "GPIO20"
A0 = "GPIO21"
A1 = "GPIO22"
D5 = "GPIO23"
D6 = "GPIO24"
T1 = "GPIO25"

[button]
D9 = ""
[switch]
D10 = ""
[led]
D13 = "red"
'''

def api(src, board=None):
    return launch(src, BOARD_FILE if board is None else board)


# Buttons, switches and LEDs on virtual pins
with api('''import board, digitalio, time
b = digitalio.DigitalInOut(board.D9); s = digitalio.DigitalInOut(board.D10)
led = digitalio.DigitalInOut(board.D13); led.switch_to_output(False)
print("ready")
last = None
while True:
    v = (b.value, s.value)
    led.value = not b.value
    if v != last:
        print("now", v)
        last = v
    time.sleep(0.01)
''') as b:
    first = seen(b, "now (True, False)")
    b.press("D9")
    pressed = seen(b, "now (False, False)")
    lit = b.part("D13")["on"]
    b.release("D9")
    released = seen(b, "now (True, False)")
    b.set("D10", 1)
    switched = seen(b, "now (True, True)")
    check("a button reads high released, low pressed; a switch toggles", first and pressed and released and switched,
          (first, pressed, released, switched))
    check("the LED the program drove, read from the panel", lit and not b.part("D13")["on"])
    trace = b.trace(since=0)["data"]
    check("trace: the button and the switch as the user moved them",
          "BUTTON D9 pressed" in trace and "BUTTON D9 released" in trace and "SWITCH D10 on" in trace, trace)
    check("trace: the LED as the program drove it", "LED D13 on" in trace and "LED D13 off" in trace, trace)

# analogio: a slider behind AnalogIn, a bar behind AnalogOut (on a pin no board file named)
ANALOG = '''import board, analogio, time
a = analogio.AnalogIn(board.A0)
o = analogio.AnalogOut(board.A1)
o.value = 12345
print("first", a.value, a.reference_voltage)
for i in range(100):
    if a.value != 32768:
        break
    time.sleep(0.05)
print("then", a.value)
'''
out, trace = run(ANALOG, "wait first\nset A0 40000\n", "then ", board=BOARD_FILE + '[analog]\nA0 = ""\n')
check("AnalogIn reads the slider: half way, then where the script put it", "first 32768 3.3" in out and "then 40000" in out, out)
check("trace: the slider and the AnalogOut", "PANEL A0 = 40000 " in trace and "ANALOG A1 = 12345 " in trace, trace)

# The key script: the same slider, from a file the program plays from its start
KEYS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "panel_keys.txt")
open(KEYS, "w").write("sleep 500\nset A0 40000\n")
with launch(ANALOG, BOARD_FILE + '[analog]\nA0 = ""\n', env=dict(HEADLESS, CIRCUITPY_SDL_KEYS=KEYS)) as b:
    check("the key script sets a slider", seen(b, "then 40000", timeout=10))
os.remove(KEYS)

# pwmio: a lamp, a servo; touchio: a pad held on the panel
out, trace = run('''import board, pwmio, touchio, time
p = pwmio.PWMOut(board.D5, frequency=1000, duty_cycle=0, variable_frequency=True)
p.duty_cycle = 32768
p.frequency = 500
s = pwmio.PWMOut(board.D6, frequency=50, duty_cycle=4915)
t = touchio.TouchIn(board.T1)
print("pwm", p.duty_cycle, p.frequency, s.frequency, t.value, t.raw_value < t.threshold)
for i in range(100):
    if t.value:
        break
    time.sleep(0.05)
print("touch", t.value, t.raw_value > t.threshold)
p.deinit()
''', "wait pwm\npress T1\n", "touch ", board=BOARD_FILE + '[pwm]\nD6 = "servo"\n[touch]\nT1 = ""\n')
check("PWMOut keeps its duty cycle and frequency; TouchIn untouched", "pwm 32768 500 50 False True" in out, out)
check("TouchIn touched while the panel holds it", "touch True True" in out, out)
check("trace: PWM duty, frequency and pulse; a servo's angle; the touch",
      re.search(r"PWM D5 duty 32768 .*\b1000 Hz", trace) and re.search(r"PWM D5 duty 32768 .*\b500 Hz", trace)
      and re.search(r"PWM D6 duty 4915 .*\b50 Hz.* 90 deg", trace) and "TOUCH T1 on" in trace and "PWM D5 off" in trace, trace)

# audiobusio: I2SOut plays through the computer, PDMIn records the panel's generator;
# audiopwmio: PWMAudioOut plays through the computer too, traced as PWM audio
AUDIO_PINS = '[pins]\nI2S_BCLK = "GPIO14"\nI2S_WS = "GPIO15"\nI2S_DATA = "GPIO16"\nMIC_CLK = "GPIO17"\nMIC_DATA = "GPIO18"\n'
out, trace = run('''import board, audiobusio, audiocore, audiopwmio, array, math, time
wave = array.array("h", [int(math.sin(i * math.pi / 8) * 20000) for i in range(16)])
sample = audiocore.RawSample(wave * 50, sample_rate=8000)
i2s = audiobusio.I2SOut(board.I2S_BCLK, board.I2S_WS, board.I2S_DATA)
i2s.play(sample)
print("playing", i2s.playing)
while i2s.playing:
    time.sleep(0.01)
print("played")
i2s.deinit()
a = audiopwmio.PWMAudioOut(board.SPEAKER)
a.play(sample)
print("pwm playing", a.playing)
while a.playing:
    time.sleep(0.01)
print("pwm played")
a.deinit()
mic = audiobusio.PDMIn(board.MIC_CLK, board.MIC_DATA, sample_rate=16000, bit_depth=16)
buf = array.array("H", [0] * 1600)
t = time.monotonic()
n = mic.record(buf, len(buf))
took = time.monotonic() - t
crossings = sum(1 for i in range(1, n) if (buf[i - 1] < 32768) != (buf[i] < 32768))
print("tone", n, max(buf) - min(buf) > 30000, 190 <= crossings <= 210, took >= 0.08)
for i in range(100):
    mic.record(buf, 160)
    if max(buf[:160]) - min(buf[:160]) < 100:
        break
    time.sleep(0.05)
mic.deinit()
mic8 = audiobusio.PDMIn(board.MIC_CLK, board.MIC_DATA, sample_rate=16000, bit_depth=8)
b8 = bytearray(160)
mic8.record(b8, 160)
print("quiet", max(buf[:160]) - min(buf[:160]) < 100, min(b8) >= 126 and max(b8) <= 130)
''', "wait tone\nset MIC_DATA 0\n", "quiet ", timeout=25, board=AUDIO_PINS + '[mic]\nMIC_DATA = "tone 1000"\n')
check("I2SOut plays a sample to its end", "playing True" in out and "played" in out, out)
check("trace: I2S play and its end", "I2S I2S_DATA play 8000 Hz 16-bit mono" in trace and "I2S I2S_DATA stop" in trace, trace)
check("PDMIn records the generator's tone: full swing, 1000 Hz, in real time", "tone 1600 True True True" in out, out)
check("PDMIn goes quiet when the panel's level is 0, 16 and 8 bits", "quiet True True" in out and "PANEL MIC_DATA = 0" in trace, out + trace)

check("PWMAudioOut plays a sample to its end", "pwm playing True" in out and "pwm played" in out, out)
check("trace: PWM audio play and stop", "PWMAUDIO SPEAKER play 8000 Hz 16-bit mono" in trace and "PWMAUDIO SPEAKER stop" in trace, trace)

# rotaryio: an encoder turned through the API, a detent at a time; divisor and position as on a board
with api('''import board, rotaryio, time
e = rotaryio.IncrementalEncoder(board.ENC_A, board.ENC_B)
print("start", e.position, e.divisor)
while e.position != 3:
    time.sleep(0.01)
print("turned", e.position)
e.position = 10
e.divisor = 2
print("set")
while e.position == 10:
    time.sleep(0.01)
print("back", e.position)
''', board='[pins]\nENC_A = "GPIO20"\nENC_B = "GPIO21"\n[encoder]\nENC_A = ""\n') as b:
    started = seen(b, "start 0 4")
    b.turn("ENC_A", 3)
    turned = seen(b, "turned 3")
    check("IncrementalEncoder counts the detents turned", started and turned, (started, turned))
    b.wait("set", timeout=5)
    b.turn("ENC_A", -1)
    check("a set position and divisor 2: a detent back counts 2", seen(b, "back 8"))
    trace = b.trace(since=0)["data"]
    check("trace: the encoder as the user turned it", "ENCODER ENC_A 3" in trace and "ENCODER ENC_A 2" in trace, trace)
print("DONE")
