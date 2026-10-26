"""pulseio on the virtual board: python3 test_pulseio.py /path/to/circuitpython

A PulseIn's pin has an IR receiver on the panel; the panel command "send NAME CODE" (or a click)
hands it a remote's NEC code. A PulseOut's pulses go to the bus trace. Checked in the program's
output and the trace.
"""
from harness import check, run


# The pulses of two codes, decoded as adafruit_irremote decodes them (MSB first, the longer
# space a one), each burst read once the pulses stop coming.
RECEIVE = '''import board, pulseio, time
p = pulseio.PulseIn(board.IR_RX, maxlen=120, idle_state=True)
print("ready", len(p), p.maxlen, p.paused)
def decode(pulses):
    spaces = pulses[3:-1:2]
    code = [0, 0, 0, 0]
    for i, s in enumerate(spaces):
        code[i // 8] = code[i // 8] << 1 | (s > 1000)
    return code
for burst in range(2):
    while len(p) == 0:
        time.sleep(0.05)
    time.sleep(0.2)
    pulses = [p.popleft() for _ in range(len(p))]
    print("burst", len(pulses), pulses[:3], decode(pulses))
p.pause()
print("paused", p.paused)
time.sleep(0.3)
print("after pause", len(p))
try:
    p.popleft()
except IndexError as e:
    print("empty", e)
print("done")
'''
out, trace = run(RECEIVE, "wait ready\nsend IR_RX\nwait burst\nsend IR_RX 0xff,2,127,128\nwait burst\n"
                 "wait paused\nsend IR_RX\n", "done", '[pins]\nIR_RX = "GPIO20"\n[ir]\nIR_RX = ""\n')
check("a PulseIn starts empty, unpaused", "ready 0 120 False" in out, out)
check("the panel's own code: the Adafruit remote's 1, NEC pulses",
      "burst 67 [9000, 4500, 560] [255, 2, 247, 8]" in out, out)
check("a code named in the key script, hex too", "burst 67 [9000, 4500, 560] [255, 2, 127, 128]" in out, out)
check("a paused PulseIn takes no pulses", "paused True" in out and "after pause 0" in out, out)
check("popleft on an empty PulseIn: IndexError", "empty pop from empty PulseIn" in out, out)
check("the trace records each code sent", trace.count("IR IR_RX ") == 3 and "IR IR_RX 255,2,127,128" in trace, trace)

# A board file's code; a PulseIn on a pin no board file names gets a receiver of its own
out, trace = run('''import board, microcontroller, pulseio, time
a = pulseio.PulseIn(board.IR_RX, maxlen=4)
b = pulseio.PulseIn(microcontroller.pin.GPIO23, maxlen=200)
print("ready")
for i in range(100):
    if len(a) and len(b):
        break
    time.sleep(0.05)
print("a", len(a), [a[i] for i in range(len(a))], a[-1])
print("b", len(b))
a.clear()
print("cleared", len(a))
print("done")
''', "wait ready\nsend IR_RX\nsend GPIO23 1,2,3,4\n", "done", '[pins]\nIR_RX = "GPIO20"\n[ir]\nIR_RX = "16,32,48,64"\n')
check("a full PulseIn keeps the newest pulses", "a 4 [560, 560, 560, 560] 560" in out, out)
check("a PulseIn on any virtual pin gets a receiver", "b 67" in out, out)
check("clear empties it", "cleared 0" in out, out)
check("the board file's code", "IR IR_RX 16,32,48,64" in trace, trace)

# PulseOut: the pulses in the trace, send() as long as they last
out, trace = run('''import array, board, pulseio, time
out = pulseio.PulseOut(board.IR_TX, frequency=38000, duty_cycle=2 ** 15)
t = time.monotonic()
out.send(array.array("H", [9000, 4500, 560, 560, 560]))
print("took", round(time.monotonic() - t, 2) >= 0.015)
print("done")
''', "", "done", '[pins]\nIR_TX = "GPIO21"\n')
check("PulseOut: the pulses in the trace", "PULSEOUT GPIO21 38000 Hz 5: 9000 4500 560 560 560" in trace, trace)
check("PulseOut: send() takes as long as the pulses", "took True" in out, out)
print("DONE")
