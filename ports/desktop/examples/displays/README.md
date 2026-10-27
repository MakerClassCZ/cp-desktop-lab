# Four displays

The window, an ST7789 TFT on SPI, an SSD1306 OLED on I2C and a 64x32 HUB75 matrix at once, all
through `displayio` with the bundle's own drivers (`adafruit_st7789`,
`adafruit_displayio_ssd1306`; `lib/` has them and `adafruit_display_text`). `board.toml` puts
the TFT and the OLED on the virtual buses; the board panel shows each as the module would look.
From a release package:

    circuitpython --drive examples/displays/CIRCUITPY --board-file examples/displays/board.toml

From the port's directory after a build:

    build-linux/circuitpython --drive examples/displays/CIRCUITPY --board-file examples/displays/board.toml

The same `code.py` drives real modules on a board with these pins.
