# This file is part of the CircuitPython project: https://circuitpython.org
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
#
# SPDX-License-Identifier: MIT

# A hosted build: libc and libm come from the system, the compiler is the host's.
CIRCUITPY_LIBC_STRING0 = 0
INTERNAL_LIBM = 0
LONGINT_IMPL = MPZ
CIRCUITPY_MIN_GCC_VERSION = 10
CIRCUITPY_BUILD_EXTENSIONS = elf

# CIRCUITPY is a FAT image file, mapped into memory (supervisor/internal_flash.c).
INTERNAL_FLASH_FILESYSTEM = 1

# stdin/stdout is the serial console; there is no USB device side.
CIRCUITPY_PORT_SERIAL = 1
CIRCUITPY_USB_DEVICE = 0

# The ADC and DAC of the MCP2221 (common-hal/analogio); the other boards refuse to construct.
CIRCUITPY_ANALOGIO = 1

# A desktop has no reset button to hold for safe mode.
CIRCUITPY_SKIP_SAFE_MODE_WAIT = 1

# No hardware behind these on a desktop or a handheld.
CIRCUITPY_BLEIO_HCI = 0
CIRCUITPY_COUNTIO = 0
CIRCUITPY_FREQUENCYIO = 0
CIRCUITPY_I2CTARGET = 0
CIRCUITPY_NVM = 0
CIRCUITPY_PULSEIO = 1
CIRCUITPY_RGBMATRIX = 1
CIRCUITPY_SDCARDIO = 0
CIRCUITPY_PARALLELDISPLAYBUS = 0

# NeoPixels: neopixel_write shows them on the board panel (panel.c), and the core
# pixel buffer the neopixel library uses where a board has it.
CIRCUITPY_NEOPIXEL_WRITE = 1
CIRCUITPY_PIXELBUF = 1

# PWM and touch pads on the virtual board's virtual pins, shown on the board panel (panel.c).
CIRCUITPY_PWMIO = 1
CIRCUITPY_TOUCHIO = 1
CIRCUITPY_TOUCHIO_USE_NATIVE = 1

# I2SOut plays through the computer as audioio does; PDMIn records the board panel's generator or
# the computer's microphone (common-hal/audiobusio).
CIRCUITPY_AUDIOBUSIO = 1
# PWM audio plays as AudioOut does; an encoder is a part of the board panel.
CIRCUITPY_AUDIOPWMIO = 1
CIRCUITPY_ROTARYIO = 1

# busio.SPI and busio.I2C through an FT232H or an MCP2221 (common-hal/busio), busio.UART on a host
# serial port (--uart). Where a board has no such bus the classes exist and refuse to construct.
CIRCUITPY_BUSIO = 1
CIRCUITPY_BUSIO_UART = 1
CIRCUITPY_BUSDEVICE = 1

# The display is an SDL2 window or screen (bindings/sdl); the SPI and I2C display buses have no
# hardware here.
CIRCUITPY_SDL = 1

# Sound is audioio.AudioOut on an SDL2 audio device (common-hal/audioio), so audiocore samples,
# audiomixer and synthio play.
CIRCUITPY_AUDIOIO = 1

# Displays on a bus: real ones on an FT232H's SPI or an I2C adapter, virtual ones on the virtual
# board's buses (spi_virtual.c, i2c_virtual.c), shown on the board panel.
CIRCUITPY_FOURWIRE = 1
CIRCUITPY_I2CDISPLAYBUS = 1
CIRCUITPY_SHARPDISPLAY = 0

# The port heap is the system allocator with a budget (supervisor/port.c); the bundled TLSF is
# 32-bit only.
CIRCUITPY_LIB_TLSF = 0

# picogame draws straight into the SDL framebuffer.
CIRCUITPY_PICOGAME = 1
CIRCUITPY_PICOGAME_FRAMEBUFFER = 1

# CIRCUITPY is memory mapped, so files can be read in place.
CIRCUITPY_STORAGE_MAP_FILE = 1

# Software-only modules a board leaves out for flash size.
CIRCUITPY_AUDIOEFFECTS = 1
CIRCUITPY_AUDIOSPEED = 1
CIRCUITPY_AUDIOFILEWRITER = 1
CIRCUITPY_AUDIOMP3 = 1
CIRCUITPY_HASHLIB = 1
CIRCUITPY_BITOPS = 1

# Networking through the host's sockets: socketpool.SocketPool(hostnetwork.HostNetwork()), with
# TLS from mbedtls.
CIRCUITPY_HOSTNETWORK = 1
CIRCUITPY_SOCKETPOOL = 1
CIRCUITPY_SSL = 1
CIRCUITPY_SSL_MBEDTLS = 1
CIRCUITPY_IPADDRESS = 1
# wifi.radio is the computer's network too (common-hal/wifi): connect() succeeds, the addresses
# are the computer's. The port has no mDNS responder.
CIRCUITPY_WIFI = 1
CIRCUITPY_MDNS = 0

# The web workflow serves CIRCUITPY over HTTP: set CIRCUITPY_WEB_API_PASSWORD (and usually
# CIRCUITPY_WEB_API_PORT, as port 80 needs root) in settings.toml.
CIRCUITPY_WEB_WORKFLOW = 1

# The MP3 decoder's inline assembly takes _WIN32 to mean MSVC; it stays out of the Windows build.
ifneq (,$(findstring mingw,$(CROSS_COMPILE)))
CIRCUITPY_AUDIOMP3 = 0
endif

# The virtual buses' device models (sensors, displays); 0 builds the virtual board without them.
CIRCUITPY_VIRTUAL_DEVICES ?= 1
