"""What a run leaves behind: python3 test_after_run.py /path/to/circuitpython

The objects free their pins themselves at the end of a run (their finalizers), as on the boards'
ports: a program that never calls deinit() can make the same objects again in the next run, and
the API's after-run report names no pin. A display kept for the terminal keeps its pins, which is
no leak.
"""

from harness import check, launch

MANY = """import board, busio, digitalio, picodvi, pulseio, pwmio, rotaryio, touchio
made = [
    digitalio.DigitalInOut(board.GPIO0),
    pwmio.PWMOut(board.GPIO1),
    touchio.TouchIn(board.GPIO2),
    rotaryio.IncrementalEncoder(board.GPIO3, board.GPIO4),
    pulseio.PulseIn(board.GPIO5),
    pulseio.PulseOut(board.GPIO6, frequency=38000),
    board.I2C(),
    board.SPI(),
    picodvi.Framebuffer(320, 240, clk_dp=board.GPIO12, clk_dn=board.GPIO13,
        red_dp=board.GPIO18, red_dn=board.GPIO19, green_dp=board.GPIO16, green_dn=board.GPIO17,
        blue_dp=board.GPIO14, blue_dn=board.GPIO15, color_depth=8),
]
print("made", len(made))
"""

with launch(MANY) as b:
    reports, outs = [], []
    for i in range(3):
        if i:
            b.reload()
        outs.append(b.wait("Code done running.", timeout=15))
        reports.append(b.after_run())
    check(
        "objects left undeinited are made again in the next run",
        all("made 9" in o and "in use" not in o for o in outs),
        outs[-1],
    )
    check(
        "the after-run report names no pin", all(r["leaked_pins"] == [] for r in reports), reports
    )
    check("the report waits for the VM's end", not any(r["vm_running"] for r in reports), reports)
    check(
        "nothing outside the heap grows from run to run",
        all(r["outside_heap_change"] == 0 for r in reports[1:]),
        reports,
    )

KEPT = """import board, displayio, framebufferio, picodvi
displayio.release_displays()
fb = picodvi.Framebuffer(320, 240, clk_dp=board.GPIO12, clk_dn=board.GPIO13,
    red_dp=board.GPIO18, red_dn=board.GPIO19, green_dp=board.GPIO16, green_dn=board.GPIO17,
    blue_dp=board.GPIO14, blue_dn=board.GPIO15, color_depth=8)
framebufferio.FramebufferDisplay(fb)
print("shown")
"""

with launch(KEPT) as b:
    first = b.wait("Code done running.", timeout=15)
    r = b.after_run()
    check(
        "a display kept for the terminal is no leak",
        "shown" in first and r["leaked_pins"] == [],
        r,
    )
    b.reload()
    again = b.wait("Code done running.", timeout=15)
    check(
        "released by the next run's release_displays(), its pins are taken again",
        "shown" in again and "in use" not in again,
        again,
    )
print("DONE")
