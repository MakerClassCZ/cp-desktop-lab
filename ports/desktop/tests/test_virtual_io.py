"""The virtual board's virtual serial port, bus pins as plain pins and the display's brightness:
python3 test_virtual_io.py /path/to/circuitpython

With no --uart, busio.UART on any free pins is a port with nothing on the other end: what the
program writes is in the bus trace, a read times out. board.TX, RX, SCL and SDA are also plain
pins, as on a microcontroller. board.DISPLAY.brightness dims the window.
"""
from harness import check, run


out, trace = run('''import board, busio, time
u = busio.UART(board.SW_X, board.SW_Y, baudrate=9600, timeout=0.3)
u.write(b"AT\\r\\n")
t = time.monotonic()
r = u.read(4)
print("read", r, round(time.monotonic() - t, 1) >= 0.3, u.in_waiting)
u.deinit()
u = busio.UART(board.TX, board.RX, baudrate=31250)
u.deinit()
print("done")
''', "", "done")
check("a virtual UART on any free pins: the write in the trace", "UART TX 4:" in trace and "AT.." in trace, trace)
check("a virtual UART: a read times out empty", "read None True 0" in out, out)
check("board.TX and board.RX make a virtual UART too", "done" in out, out)

out, trace = run('''import board, digitalio, touchio
for name in ("TX", "RX", "SCL", "SDA"):
    d = digitalio.DigitalInOut(getattr(board, name))
    d.switch_to_output(value=True)
    print(name, d.value)
    d.deinit()
t = touchio.TouchIn(board.TX)
print("touch", t.value)
print("done")
''', "", "done")
check("TX, RX, SCL and SDA are plain pins too", all("%s True" % n in out for n in ("TX", "RX", "SCL", "SDA")), out)
check("touchio on board.TX", "touch False" in out, out)

out, trace = run('''import board
print("brightness", board.DISPLAY.brightness)
board.DISPLAY.brightness = 0.25
print("now", board.DISPLAY.brightness)
''', "", "now")
check("board.DISPLAY.brightness reads and sets", "brightness 1.0" in out and "now 0.25" in out, out)
print("DONE")
