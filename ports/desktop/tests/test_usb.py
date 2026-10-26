"""usb_hid and usb_midi on a host: python3 test_usb.py /path/to/circuitpython

The port is no USB device; the modules are there so code for a board runs, and what it sends goes
to the bus trace: HID reports and MIDI bytes. Nothing reaches the computer's keyboard or MIDI.
"""
from harness import check, run


# usb_hid: the default devices, found as adafruit_hid finds them, reports traced
out, trace = run('''import usb_hid
kinds = [(d.usage_page, d.usage) for d in usb_hid.devices]
print("devices", kinds)
print("same", usb_hid.Device.KEYBOARD in usb_hid.devices, usb_hid.Device.MOUSE is usb_hid.devices[1])
kbd = [d for d in usb_hid.devices if d.usage_page == 1 and d.usage == 6][0]
kbd.send_report(bytes([2, 0, 4, 0, 0, 0, 0, 0]))
kbd.send_report(bytes(8))
usb_hid.Device.MOUSE.send_report(bytes([1, 5, 0xfb, 0]))
usb_hid.Device.CONSUMER_CONTROL.send_report(bytes([0xe9, 0]))
print("leds", kbd.get_last_received_report(), usb_hid.get_boot_device())
try:
    kbd.send_report(bytes(3))
except ValueError as e:
    print("short", e)
usb_hid.enable((usb_hid.Device.KEYBOARD,))
usb_hid.disable()
print("hid done")
''', "", "hid done")
check("usb_hid: keyboard, mouse and consumer control", "devices [(1, 6), (1, 2), (12, 1)]" in out and "same True True" in out, out)
check("usb_hid: no report from the host, no boot device", "leds None 0" in out, out)
check("usb_hid: a report of the wrong length is refused", "short" in out and "hid done" in out, out)
check("trace: HID reports",
      "HID KEYBOARD 02 00 04 00 00 00 00 00" in trace and "HID KEYBOARD 00 00 00 00 00 00 00 00" in trace
      and "HID MOUSE 01 05 fb 00" in trace and "HID CONSUMER_CONTROL e9 00" in trace, trace)

# usb_midi: an input with nothing to read, an output whose bytes are traced
out, trace = run('''import usb_midi
pin, pout = usb_midi.ports
print("ports", type(pin).__name__, type(pout).__name__)
print("read", pin.read(3), pin.readinto(bytearray(3)))
print("wrote", pout.write(bytes([0x90, 60, 100])))
usb_midi.set_names(streaming_interface_name="x")
usb_midi.enable()
print("midi done")
''', "", "midi done")
check("usb_midi: PortIn and PortOut", "ports PortIn PortOut" in out, out)
check("usb_midi: nothing to read, a write writes", "read None None" in out and "wrote 3" in out, out)
check("trace: MIDI bytes out", "MIDI OUT 90 3c 64" in trace, trace)
print("DONE")
