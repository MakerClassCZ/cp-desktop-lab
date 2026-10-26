"""busio.UART on a host serial port: python3 test_uart.py /path/to/circuitpython

On Linux, socat makes a pair of linked pseudo-terminals; the program gets one (--uart), the test
talks on the other. Skips where there is no socat (and on Windows, where the MCP2221 test covers
the UART with a wire between its TX and RX).
"""
import os, select, shutil, subprocess, sys, time
from harness import Run, T, check, drive
TRACE = os.path.join(T, "uart_trace.txt")

if os.name == "nt" or shutil.which("socat") is None:
    print("SKIP no socat (Linux only)")
    sys.exit(0)

import tty
A = os.path.join(T, "ttyA")
B = os.path.join(T, "ttyB")
for p in (A, B):
    if os.path.lexists(p):
        os.remove(p)
socat = subprocess.Popen(["socat", "pty,raw,echo=0,link=" + A, "pty,raw,echo=0,link=" + B],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
end = time.time() + 5
while time.time() < end and not (os.path.exists(A) and os.path.exists(B)):
    time.sleep(0.05)
peer = os.open(B, os.O_RDWR | os.O_NOCTTY)
tty.setraw(peer)

def peer_read(n, timeout=5):
    got = b""
    end = time.time() + timeout
    while len(got) < n and time.time() < end:
        r, _, _ = select.select([peer], [], [], 0.1)
        if r:
            got += os.read(peer, n - len(got))
    return got

def run(src, args=("--uart", A)):
    return Run(["--drive", drive(src), "--board", "virtual"] + list(args))

try:
    # Both ways, and the trace of both.
    if os.path.exists(TRACE):
        os.remove(TRACE)
    r = run('''import board, busio
uart = busio.UART(board.TX, board.RX, baudrate=115200, timeout=2)
uart.write(b"hello\\n")
print("sent")
data = uart.read(5)
print("got", data)
''', args=("--uart", A, "--trace-bus", TRACE))
    check("the program writes to the port", peer_read(6) == b"hello\n")
    r.wait_for("sent")
    os.write(peer, b"world")
    check("the program reads from the port", r.wait_for("got b'world'"), r.text())
    r.stop()
    trace = open(TRACE).read() if os.path.exists(TRACE) else ""
    check("trace: UART TX and RX", "UART TX 6: 68 65 6c 6c 6f 0a  hello." in trace and "UART RX 5: 77 6f 72 6c 64  world" in trace, trace)

    # A read times out with what it has, None when nothing came.
    r = run('''import board, busio, time
uart = busio.UART(board.TX, board.RX, timeout=0.5)
t = time.monotonic()
r = uart.read(10)
print("timeout", r, round(time.monotonic() - t, 1))
''')
    check("a read with nothing times out with None", r.wait_for("timeout None 0.5"), r.text())
    r.stop()

    # in_waiting, reset_input_buffer, the baud rate.
    r = run('''import board, busio, time
uart = busio.UART(board.TX, board.RX, baudrate=9600, timeout=1)
print("ready")
while uart.in_waiting < 3:
    time.sleep(0.05)
w = uart.in_waiting
uart.reset_input_buffer()
uart.baudrate = 57600
print("waiting", w, uart.in_waiting, uart.baudrate)
''')
    r.wait_for("ready")
    os.write(peer, b"abc")
    check("in_waiting, reset_input_buffer and baudrate", r.wait_for("waiting 3 0 57600"), r.text())
    r.stop()

    # Refusals: swapped pins, flow control.
    r = run('''import board, busio
for args, kw in (((board.RX, board.TX), {}), ((board.TX, board.RX), {"rts": board.SW_A})):
    try:
        busio.UART(*args, **kw)
        print("accepted")
    except Exception as e:
        print("refused", type(e).__name__)
print("done")
''')
    r.wait_for("done")
    out = r.text()
    check("swapped pins and flow control are refused", out.count("refused ValueError") == 2 and "accepted" not in out, out)
    r.stop()

    # A deinit frees TX and RX: the same UART can be made again, board.UART() too.
    r = run('''import board, busio
u = busio.UART(board.TX, board.RX)
u.deinit()
with busio.UART(board.TX, board.RX) as u2:
    pass
b = board.UART()
b.deinit()
print("again", board.UART() is not None)
''')
    check("a deinit frees TX and RX for the next UART", r.wait_for("again True"), r.text())
    r.stop()

    # The other end hangs up: a read ends with an error instead of spinning until the timeout.
    r = run('''import board, busio, time
u = busio.UART(board.TX, board.RX, timeout=5)
print("reading")
t = time.monotonic()
try:
    r = u.read(10)
    print("hangup read", r, round(time.monotonic() - t))
except OSError as e:
    print("hangup error", time.monotonic() - t < 3)
''')
    r.wait_for("reading")
    time.sleep(0.5)
    os.close(peer)
    socat.terminate()
    socat.wait()
    check("a hang-up ends the read early", r.wait_for("hangup error True", 10), r.text())
    r.stop()
    peer = os.open("/dev/null", os.O_RDWR)

    # A port that does not exist: an OSError at construction, not at start.
    r = run('''import board, busio
try:
    busio.UART(board.TX, board.RX)
except OSError as e:
    print("oserror", "/nonexistent/tty" in str(e))
''', args=("--uart", "/nonexistent/tty"))
    check("a missing port is an OSError naming it", r.wait_for("oserror True"), r.text())
    r.stop()

    # TX is a plain pin on the virtual board too, but not while the UART holds it.
    r = run('''import board, busio, digitalio
u = busio.UART(board.TX, board.RX)
try:
    digitalio.DigitalInOut(board.TX)
    print("dio accepted")
except ValueError:
    print("dio refused")
''', args=())
    check("digitalio on the UART's TX is refused", r.wait_for("dio refused"), r.text())
    r.stop()
finally:
    os.close(peer)
    socat.terminate()
    socat.wait()
print("DONE")
