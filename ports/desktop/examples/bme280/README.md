# A BME280 without a BME280

`code.py` is the BME280 Learning Guide's example, unchanged. The virtual board has no BME280, so
one is plugged in at 0x77 when the library asks for it (the terminal says so) and the board panel
shows its temperature, humidity and pressure sliders; moving one changes what the program prints.

    circuitpython --drive examples/bme280/CIRCUITPY

The REPL reaches the sensor too: `board.attached["BME280"].temp = 35`.

`graph.py` is the same sensor on the display: the temperature in large digits and the last minute
as bars, blue to red. Copy it over `code.py` while the program runs and it reloads into the graph:

    cp examples/bme280/CIRCUITPY/graph.py examples/bme280/CIRCUITPY/code.py

`lib/` has `adafruit_bme280` and `adafruit_display_text` from the bundle. The same files run on a
board with a real BME280, or here with one through an FT232H (`--board ft232h`).
