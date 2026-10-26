"""An RGB matrix on the virtual board: python3 test_rgbmatrix.py /path/to/circuitpython

rgbmatrix.RGBMatrix (as adafruit_matrixportal builds it, through framebufferio) shows on the
panel as the display MATRIX; the test reads its picture through the control API.
"""
from harness import check, launch, picture, seen

PINS = '''[pins]
MTX_R1 = "GPIO14"
MTX_G1 = "GPIO15"
MTX_B1 = "GPIO16"
MTX_R2 = "GPIO17"
MTX_G2 = "GPIO18"
MTX_B2 = "GPIO19"
MTX_ADDRA = "GPIO20"
MTX_ADDRB = "GPIO21"
MTX_ADDRC = "GPIO22"
MTX_ADDRD = "GPIO23"
MTX_CLK = "GPIO25"
MTX_LAT = "GPIO26"
MTX_OE = "GPIO27"
'''

with launch('''import board, displayio, framebufferio, rgbmatrix, time
displayio.release_displays()
matrix = rgbmatrix.RGBMatrix(width=64, bit_depth=4,
    rgb_pins=[board.MTX_R1, board.MTX_G1, board.MTX_B1, board.MTX_R2, board.MTX_G2, board.MTX_B2],
    addr_pins=[board.MTX_ADDRA, board.MTX_ADDRB, board.MTX_ADDRC, board.MTX_ADDRD],
    clock_pin=board.MTX_CLK, latch_pin=board.MTX_LAT, output_enable_pin=board.MTX_OE)
print("matrix", matrix.width, matrix.height)
display = framebufferio.FramebufferDisplay(matrix, auto_refresh=False)
group = displayio.Group()
background = displayio.Bitmap(64, 32, 2)
palette = displayio.Palette(2)
palette[0] = 0x0000FF
palette[1] = 0xFF0000
square = displayio.Bitmap(8, 8, 2)
square.fill(1)
group.append(displayio.TileGrid(background, pixel_shader=palette))
group.append(displayio.TileGrid(square, pixel_shader=palette, x=56, y=24))
display.root_group = group
display.refresh()
print("drawn")
time.sleep(0.5)
matrix.brightness = 0
print("dark", matrix.brightness)
while True:
    time.sleep(1)
''', PINS) as b:
    seen(b, "drawn")
    img = picture(b, "MATRIX")
    seen(b, "dark")
    dark = picture(b, "MATRIX")
    out = b.console(since=0)["data"]
check("RGBMatrix: 64 wide, 32 high from four address pins", "matrix 64 32" in out, out)
check("the panel's MATRIX is 64x32", img is not None and img[:2] == (64, 32), img and img[:2])
if img:
    w, h, px = img
    check("the red square at the bottom right", px(60, 28)[0] > 200 and px(60, 28)[2] < 50, px(60, 28))
    check("blue elsewhere", px(2, 2)[2] > 200 and px(2, 2)[0] < 50, px(2, 2))
check("brightness 0 darkens the matrix", "dark 0.0" in out and dark[2](2, 2) == (0, 0, 0), (out, dark[2](2, 2)))
print("DONE")
