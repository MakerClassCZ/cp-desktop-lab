# Parts plugged in from the program: a button, an LED and a BME280 sensor (board.parts).
import time

import board
import digitalio
from microcontroller import pin

board.attach(board.parts.Button(pin.GPIO16, name="BUTTON"))
board.attach(board.parts.LED(pin.GPIO17, color="green", name="LAMP"))
env = board.attach(board.parts.BME280(name="ENV"))

key = digitalio.DigitalInOut(pin.GPIO16)
key.pull = digitalio.Pull.UP
lamp = digitalio.DigitalInOut(pin.GPIO17)
lamp.switch_to_output()

i2c = board.I2C()
while not i2c.try_lock():
    pass
print("on the I2C bus:", [hex(address) for address in i2c.scan()])
i2c.unlock()

print("Hold BUTTON on the panel, move ENV.temp's slider.")
last = None
while True:
    lamp.value = not key.value  # pressed reads low
    if env.temp != last:
        last = env.temp
        print("temperature", last, "C")
    time.sleep(0.05)
