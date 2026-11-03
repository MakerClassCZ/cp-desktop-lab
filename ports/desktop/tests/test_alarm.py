"""alarm: python3 test_alarm.py /path/to/circuitpython

A light sleep wakes on a time or on a pin (a button pressed through the control API) and returns
that alarm; a deep sleep ends the program and starts it again from the top, alarm.wake_alarm
saying what woke it, alarm.sleep_memory kept, the reset reason a deep sleep alarm's.
"""

import http.client, time
from harness import check, launch, run, seen

out, _ = run(
    """import alarm, time
a = alarm.time.TimeAlarm(monotonic_time=time.monotonic() + 0.5)
t = time.monotonic()
w = alarm.light_sleep_until_alarms(a)
print("woke", w is a, alarm.wake_alarm is a, round(time.monotonic() - t, 1))
""",
    expect="woke",
)
check("light sleep: a time alarm wakes it, after its time", "woke True True 0.5" in out, out)

out, _ = run(
    """import alarm, board
a = alarm.pin.PinAlarm(pin=board.BTN, value=False, pull=True)
print("sleeping")
w = alarm.light_sleep_until_alarms(a)
print("woke", w is a)
""",
    "wait sleeping\npress BTN\n",
    "woke",
    board='[pins]\nBTN = "GPIO4"\n[button]\nBTN = ""\n',
)
check("light sleep: a pin alarm wakes it when the button is pressed", "woke True" in out, out)

out, _ = run(
    """import alarm, microcontroller, time
if alarm.wake_alarm is None:
    alarm.sleep_memory[0] = 42
    print("sleeping")
    alarm.exit_and_deep_sleep_until_alarms(alarm.time.TimeAlarm(monotonic_time=time.monotonic() + 1))
else:
    print("woke", type(alarm.wake_alarm).__name__, alarm.sleep_memory[0],
          microcontroller.cpu.reset_reason == microcontroller.ResetReason.DEEP_SLEEP_ALARM)
""",
    expect="woke",
    timeout=20,
)
check(
    "deep sleep: the program starts again once it is over, woken by its alarm",
    "woke TimeAlarm" in out,
    out,
)
check("deep sleep: sleep_memory kept", "woke TimeAlarm 42" in out, out)
check("deep sleep: the reset reason is the alarm's", "woke TimeAlarm 42 True" in out, out)

# A key on the console during a deep sleep is the reset button: the board starts again unwoken.
with launch(
    """import alarm, time
print("wake", alarm.wake_alarm)
if alarm.wake_alarm is None and not alarm.sleep_memory[1]:
    alarm.sleep_memory[1] = 1
    alarm.exit_and_deep_sleep_until_alarms(alarm.time.TimeAlarm(monotonic_time=time.monotonic() + 60))
print("after the reset button", alarm.sleep_memory[1])
"""
) as b:
    seen(b, "wake None", 10)
    time.sleep(1)
    try:
        b.write("x")
    except (ConnectionError, http.client.HTTPException):
        pass  # the board resets while it answers: the key came
    # The console of the board started again, from its beginning.
    console, end = "", time.time() + 15
    while "after the reset button" not in console and time.time() < end:
        time.sleep(0.5)
        try:
            console = b.console(since=0)["data"]
        except (ConnectionError, http.client.HTTPException):
            pass
check(
    "deep sleep: a key on the console starts the board again, sleep memory kept",
    "after the reset button 1" in console,
    console[-300:],
)
print("DONE")
