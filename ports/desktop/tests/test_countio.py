"""countio on the virtual board: python3 test_countio.py /path/to/circuitpython

A Counter counts the edges of the level the panel puts on its pin (a button pressed and let go),
and the panel line "count PIN N" adds N edges at once.
"""

from harness import check, run

PROGRAM = """import board, countio, digitalio, lab
falls = countio.Counter(board.BTN, edge=countio.Edge.FALL, pull=digitalio.Pull.UP)
both = countio.Counter(board.GPIO6, edge=countio.Edge.RISE_AND_FALL)
print("start", falls.count, both.count)
for _ in range(3):
    lab.panel("press BTN", "release BTN")
print("presses", falls.count)
lab.panel("count BTN 10", "count GPIO6 4")
print("counted", falls.count, both.count)
falls.reset()
print("reset", falls.count)
for line in ("count GPIO7 1", "count BTN", "count BTN -2"):
    try:
        lab.panel(line)
    except ValueError as e:
        print("refused:", e)
falls.deinit()
try:
    lab.panel("count BTN 1")
except ValueError as e:
    print("after deinit:", e)
print("done")
"""

out, trace = run(PROGRAM, "", "done", '[pins]\nBTN = "GPIO5"\n[button]\nBTN = ""\n')
check("a Counter starts at 0", "start 0 0" in out, out)
check("a button pressed three times: three falling edges", "presses 3" in out, out)
check("count PIN N: N edges, by any name of the pin", "counted 13 4" in out, out)
check("reset() starts again at 0", "reset 0" in out, out)
check(
    "count: no Counter there, no number, a negative one",
    "refused: no Counter on GPIO7" in out and out.count("refused: count needs") == 2,
    out,
)
check("a Counter deinited counts no more", "after deinit: no Counter on BTN" in out, out)
check("count is traced", "COUNT BTN +10" in trace and "COUNT GPIO6 +4" in trace, trace)
