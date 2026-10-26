// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// --board gpiochip: real pins on a Linux computer that has them (a Raspberry Pi and the like),
// through the kernel's own interfaces and no library: the GPIO character device (uAPI v2, one
// line request per pin), an i2c-dev bus and a spidev device. Pin numbers are the chip's line
// offsets, which on a Raspberry Pi are the BCM numbers of its header.

#include "py/mpconfig.h"
#include "py/mperrno.h"

#include <stdio.h>
#include <string.h>

#include "desktop_pins.h"
#include "desktop_port.h"

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/spi/spidev.h>

#define I2C_SCL 3
#define I2C_SDA 2
#define SPI_SCK 11
#define SPI_MOSI 10
#define SPI_MISO 9

static int _chip = -1;
static uint32_t _lines;                     // the chip's line count
static int _line[DESKTOP_PIN_COUNT];          // the request of each pin in use, -1 when none
static uint32_t _output;                    // pins configured as outputs
static uint32_t _value;                     // their levels
static uint32_t _bus_pins;                  // pins a kernel bus driver has: not for digitalio

// On a Pi the pin controller hands a pin to whoever asks, so a GPIO request on a bus pin would
// take it from the I2C, SPI or UART driver until reboot. Those pins are refused while their
// device exists.
static uint32_t _find_bus_pins(void) {
    uint32_t mask = 0;
    char path[32];
    snprintf(path, sizeof(path), "/dev/i2c-%d", desktop_options.i2c_bus);
    if (access(path, F_OK) == 0) {
        mask |= (1u << I2C_SDA) | (1u << I2C_SCL);
    }
    if (access(desktop_options.spidev, F_OK) == 0) {
        mask |= (1u << 7) | (1u << 8) | (1u << SPI_MISO) | (1u << SPI_MOSI) | (1u << SPI_SCK);
    }
    if (access("/dev/serial0", F_OK) == 0) {
        mask |= (1u << 14) | (1u << 15);
    }
    return mask;
}

static bool _present(void) {
    return access(desktop_options.gpiochip, F_OK) == 0;
}

static bool _open(void) {
    for (int i = 0; i < DESKTOP_PIN_COUNT; i++) {
        _line[i] = -1;
    }
    _chip = open(desktop_options.gpiochip, O_RDWR | O_CLOEXEC);
    if (_chip < 0) {
        fprintf(stderr, "circuitpython: cannot open %s: %s\n", desktop_options.gpiochip, strerror(errno));
        fprintf(stderr, "  --board gpiochip needs the kernel's GPIO character device, as on a Raspberry Pi;\n"
            "  --gpiochip names another chip, and the user needs access to it (the gpio group)\n");
        return false;
    }
    struct gpiochip_info info;
    if (ioctl(_chip, GPIO_GET_CHIPINFO_IOCTL, &info) != 0) {
        fprintf(stderr, "circuitpython: %s is no GPIO chip: %s\n", desktop_options.gpiochip, strerror(errno));
        close(_chip);
        _chip = -1;
        return false;
    }
    _lines = info.lines;
    _bus_pins = _find_bus_pins();
    _output = 0;
    _value = 0;
    return true;
}

static void _release(uint8_t pin) {
    if (_line[pin] >= 0) {
        // An input again before the kernel gets the line back, so nothing stays driven.
        struct gpio_v2_line_config config = { .flags = GPIO_V2_LINE_FLAG_INPUT };
        ioctl(_line[pin], GPIO_V2_LINE_SET_CONFIG_IOCTL, &config);
        close(_line[pin]);
        _line[pin] = -1;
    }
    _output &= ~(1u << pin);
}

static void _reset(uint32_t keep_mask) {
    for (uint8_t pin = 0; pin < DESKTOP_PIN_COUNT; pin++) {
        if (!(keep_mask & (1u << pin))) {
            _release(pin);
        }
    }
}

static int _neopixel_spi = -1;              // NeoPixels' own spidev: busio.SPI may set other modes

static void _close(void) {
    if (_chip >= 0) {
        _reset(0);
        close(_chip);
        _chip = -1;
    }
    if (_neopixel_spi >= 0) {
        close(_neopixel_spi);
        _neopixel_spi = -1;
    }
}

// MOSI while spidev has it: a digitalio pin only to carry NeoPixel data (the neopixel library
// makes a DigitalInOut of its pin); taking the line from SPI would end SPI until reboot.
static bool _neopixel_pin(uint8_t pin) {
    return pin == SPI_MOSI && (_bus_pins & (1u << SPI_MOSI));
}

static bool _pin_ok(uint8_t pin) {
    return pin < DESKTOP_PIN_COUNT && pin < _lines && (!(_bus_pins & (1u << pin)) || _neopixel_pin(pin));
}

static digitalinout_result_t _configure(uint8_t pin, bool output, bool open_drain, digitalio_pull_t pull) {
    if (!_pin_ok(pin) || _neopixel_pin(pin)) {
        return DIGITALINOUT_OK;             // a bus pin past the chip's lines, or MOSI: nothing to set
    }
    struct gpio_v2_line_config config = { 0 };
    config.flags = output ? GPIO_V2_LINE_FLAG_OUTPUT : GPIO_V2_LINE_FLAG_INPUT;
    if (output && open_drain) {
        config.flags |= GPIO_V2_LINE_FLAG_OPEN_DRAIN;
    }
    if (!output) {
        config.flags |= pull == PULL_UP ? GPIO_V2_LINE_FLAG_BIAS_PULL_UP
            : pull == PULL_DOWN ? GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN : GPIO_V2_LINE_FLAG_BIAS_DISABLED;
    }
    if (output) {
        // The level the pin starts with: the one last written.
        config.num_attrs = 1;
        config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        config.attrs[0].attr.values = (_value >> pin) & 1;
        config.attrs[0].mask = 1;
    }
    int status;
    if (_line[pin] < 0) {
        struct gpio_v2_line_request request = { 0 };
        request.offsets[0] = pin;
        request.num_lines = 1;
        request.config = config;
        snprintf(request.consumer, sizeof(request.consumer), "circuitpython");
        status = ioctl(_chip, GPIO_V2_GET_LINE_IOCTL, &request);
        if (status == 0) {
            _line[pin] = request.fd;
        }
    } else {
        status = ioctl(_line[pin], GPIO_V2_LINE_SET_CONFIG_IOCTL, &config);
    }
    if (status != 0) {
        if (errno == EBUSY) {
            return DIGITALINOUT_PIN_BUSY;   // the kernel uses it (I2C, SPI, a driver)
        }
        return pull != PULL_NONE ? DIGITALINOUT_INVALID_PULL : DIGITALINOUT_PIN_BUSY;
    }
    if (output) {
        _output |= 1u << pin;
    } else {
        _output &= ~(1u << pin);
    }
    return DIGITALINOUT_OK;
}

static bool _write(uint8_t pin, bool value) {
    if (value) {
        _value |= 1u << pin;
    } else {
        _value &= ~(1u << pin);
    }
    if (pin >= DESKTOP_PIN_COUNT || _line[pin] < 0 || !(_output & (1u << pin))) {
        return true;
    }
    struct gpio_v2_line_values values = { .bits = value ? 1 : 0, .mask = 1 };
    return ioctl(_line[pin], GPIO_V2_LINE_SET_VALUES_IOCTL, &values) == 0;
}

static bool _read(uint8_t pin, bool *level) {
    if (pin >= DESKTOP_PIN_COUNT || _line[pin] < 0) {
        *level = false;
        return true;
    }
    struct gpio_v2_line_values values = { .mask = 1 };
    if (ioctl(_line[pin], GPIO_V2_LINE_GET_VALUES_IOCTL, &values) != 0) {
        return false;
    }
    *level = (values.bits & 1) != 0;
    return true;
}

// ---- I2C: /dev/i2c-N ----

static int _i2c = -1;
static uint8_t *_pending;                   // a write left without a stop, sent with the next read
static size_t _pending_len;
static uint16_t _pending_addr;

#define I2C_MAX 8192                        // i2c-dev's limit for one message

static int _i2c_errno(void) {
    return errno == ENXIO || errno == EREMOTEIO ? -MP_ENODEV : errno == ETIMEDOUT ? -MP_ETIMEDOUT : -MP_EIO;
}

static bool _i2c_construct(uint32_t frequency, uint32_t *actual_hz) {
    if (_i2c < 0) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/i2c-%d", desktop_options.i2c_bus);
        _i2c = open(path, O_RDWR | O_CLOEXEC);
        if (_i2c < 0) {
            snprintf(desktop_bus_error, sizeof(desktop_bus_error), "cannot open %s: %s", path, strerror(errno));
            return false;
        }
    }
    // The bus speed is the kernel's (the device tree), not the program's.
    *actual_hz = frequency;
    return true;
}

static void _i2c_deinit(void) {
    if (_i2c >= 0) {
        close(_i2c);
        _i2c = -1;
    }
    free(_pending);
    _pending = NULL;
    _pending_len = 0;
}

static int _i2c_rdwr(struct i2c_msg *msgs, int count) {
    struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = (uint32_t)count };
    return ioctl(_i2c, I2C_RDWR, &data) < 0 ? _i2c_errno() : 0;
}

static int _i2c_write(uint16_t address, const uint8_t *data, size_t len, bool stop) {
    if (_i2c < 0) {
        return -MP_EIO;
    }
    if (len > I2C_MAX) {
        return -MP_EINVAL;
    }
    if (!stop) {
        // Kept for the read that follows: one combined transfer, so a repeated start. Nothing to
        // keep for an empty write: some adapters refuse a zero-length message.
        if (len == 0) {
            return 0;
        }
        uint8_t *copy = realloc(_pending, len);
        if (copy == NULL) {
            return -MP_ENOMEM;
        }
        memcpy(copy, data, len);
        _pending = copy;
        _pending_len = len;
        _pending_addr = address;
        return 0;
    }
    if (len == 0) {
        // A probe: an SMBus quick write. An address a kernel driver has bound is there (EBUSY).
        if (ioctl(_i2c, I2C_SLAVE, address) < 0) {
            return errno == EBUSY ? 0 : -MP_EIO;
        }
        struct i2c_smbus_ioctl_data quick = { .read_write = I2C_SMBUS_WRITE, .command = 0, .size = I2C_SMBUS_QUICK, .data = NULL };
        return ioctl(_i2c, I2C_SMBUS, &quick) < 0 ? _i2c_errno() : 0;
    }
    struct i2c_msg msg = { .addr = address, .flags = 0, .len = (uint16_t)len, .buf = (uint8_t *)data };
    return _i2c_rdwr(&msg, 1);
}

static int _i2c_read(uint16_t address, uint8_t *data, size_t len) {
    if (_i2c < 0) {
        return -MP_EIO;
    }
    if (len > I2C_MAX) {
        return -MP_EINVAL;
    }
    struct i2c_msg msgs[2];
    int count = 0;
    if (_pending != NULL && _pending_addr == address) {
        msgs[count++] = (struct i2c_msg) { .addr = address, .flags = 0, .len = (uint16_t)_pending_len, .buf = _pending };
    }
    msgs[count++] = (struct i2c_msg) { .addr = address, .flags = I2C_M_RD, .len = (uint16_t)len, .buf = data };
    int status = _i2c_rdwr(msgs, count);
    free(_pending);
    _pending = NULL;
    _pending_len = 0;
    return status;
}

static const desktop_i2c_ops_t _i2c_ops = {
    .scl = I2C_SCL,
    .sda = I2C_SDA,
    .construct = _i2c_construct,
    .deinit = _i2c_deinit,
    .write = _i2c_write,
    .read = _i2c_read,
};

// ---- SPI: /dev/spidevB.C ----

#define SPI_CHUNK 4096                      // spidev's default buffer size

static int _spi = -1;
static uint32_t _spi_hz = 250000;

static bool _spi_construct(bool has_mosi, bool has_miso) {
    (void)has_mosi;
    (void)has_miso;
    if (_spi < 0) {
        _spi = open(desktop_options.spidev, O_RDWR | O_CLOEXEC);
        if (_spi < 0) {
            snprintf(desktop_bus_error, sizeof(desktop_bus_error), "cannot open %s: %s", desktop_options.spidev, strerror(errno));
            return false;
        }
    }
    _spi_hz = 250000;
    return true;
}

static void _spi_deinit(void) {
    if (_spi >= 0) {
        close(_spi);
        _spi = -1;
    }
}

static desktop_spi_result_t _spi_configure(uint32_t hz, uint8_t polarity, uint8_t phase, uint8_t bits, uint32_t *actual_hz) {
    if (bits != 8) {
        return DESKTOP_SPI_BAD_BITS;
    }
    uint8_t mode = (uint8_t)((polarity ? SPI_CPOL : 0) | (phase ? SPI_CPHA : 0));
    uint8_t word = 8;
    if (ioctl(_spi, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(_spi, SPI_IOC_WR_BITS_PER_WORD, &word) < 0
        || ioctl(_spi, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
        return DESKTOP_SPI_FAILED;
    }
    _spi_hz = hz;
    *actual_hz = hz;
    return DESKTOP_SPI_OK;
}

static bool _spi_transfer(const uint8_t *out, uint8_t fill, uint8_t *in, size_t len) {
    static uint8_t fills[SPI_CHUNK];
    if (out == NULL) {
        memset(fills, fill, sizeof(fills));
    }
    while (len > 0) {
        size_t n = len < SPI_CHUNK ? len : SPI_CHUNK;
        struct spi_ioc_transfer t = { 0 };
        t.tx_buf = (uintptr_t)(out != NULL ? out : fills);
        t.rx_buf = (uintptr_t)in;
        t.len = (uint32_t)n;
        t.speed_hz = _spi_hz;
        t.bits_per_word = 8;
        if (ioctl(_spi, SPI_IOC_MESSAGE(1), &t) < 0) {
            return false;
        }
        if (out != NULL) {
            out += n;
        }
        if (in != NULL) {
            in += n;
        }
        len -= n;
    }
    return true;
}

// ---- NeoPixels: WS2812 data on MOSI, each bit a byte of SPI at 6.4 MHz ----

#define NEOPIXEL_HZ 6400000
#define NEOPIXEL_ONE 0xf8                   // 781 ns high, then low: a one
#define NEOPIXEL_ZERO 0xc0                  // 312 ns high: a zero

// What spidev moves in one message (its bufsiz parameter): a gap between messages would latch
// the pixels halfway.
// Read once: a module parameter, fixed while the kernel runs.
static size_t _spidev_bufsiz(void) {
    static size_t size;
    if (size == 0) {
        size = 4096;
        FILE *f = fopen("/sys/module/spidev/parameters/bufsiz", "r");
        if (f != NULL) {
            unsigned long v;
            if (fscanf(f, "%lu", &v) == 1 && v > 0) {
                size = v;
            }
            fclose(f);
        }
    }
    return size;
}

static bool _neopixel(uint8_t pin, const uint8_t *pixels, size_t len, const char **why) {
    static char message[160];
    if (!_neopixel_pin(pin)) {
        *why = "NeoPixels go out on MOSI (board.MOSI, D10) through spidev here: enable SPI (dtparam=spi=on)";
        return false;
    }
    size_t bytes = 1 + len * 8;             // a low byte first, so the first bit starts clean
    if (bytes > _spidev_bufsiz()) {
        snprintf(message, sizeof(message), "%u NeoPixel bytes are more than spidev sends at once "
            "(%u pixels of 3 bytes); spidev.bufsiz=65536 on the kernel command line raises it",
            (unsigned)len, (unsigned)((_spidev_bufsiz() - 1) / 24));
        *why = message;
        return false;
    }
    if (_neopixel_spi < 0) {
        _neopixel_spi = open(desktop_options.spidev, O_RDWR | O_CLOEXEC);
        if (_neopixel_spi < 0) {
            snprintf(message, sizeof(message), "cannot open %s: %s", desktop_options.spidev, strerror(errno));
            *why = message;
            return false;
        }
    }
    uint8_t *out = malloc(bytes);
    if (out == NULL) {
        *why = "out of memory";
        return false;
    }
    out[0] = 0;
    for (size_t i = 0; i < len; i++) {
        for (int b = 0; b < 8; b++) {
            out[1 + i * 8 + b] = (pixels[i] & (0x80 >> b)) ? NEOPIXEL_ONE : NEOPIXEL_ZERO;
        }
    }
    uint8_t mode = SPI_MODE_0;
    struct spi_ioc_transfer t = { 0 };
    t.tx_buf = (uintptr_t)out;
    t.len = (uint32_t)bytes;
    t.speed_hz = NEOPIXEL_HZ;
    t.bits_per_word = 8;
    bool ok = ioctl(_neopixel_spi, SPI_IOC_WR_MODE, &mode) >= 0 && ioctl(_neopixel_spi, SPI_IOC_MESSAGE(1), &t) >= 0;
    if (!ok) {
        snprintf(message, sizeof(message), "%s: %s", desktop_options.spidev, strerror(errno));
        *why = message;
    }
    free(out);
    return ok;
}

static const desktop_spi_ops_t _spi_ops = {
    .sck = SPI_SCK,
    .mosi = SPI_MOSI,
    .miso = SPI_MISO,
    .construct = _spi_construct,
    .deinit = _spi_deinit,
    .configure = _spi_configure,
    .transfer = _spi_transfer,
};

const desktop_pin_backend_t gpiochip_backend = {
    .name = "GPIO",
    .present = _present,
    .open = _open,
    .close = _close,
    .reset = _reset,
    .configure = _configure,
    .write = _write,
    .read = _read,
    .lost = NULL,
    .pin_ok = _pin_ok,
    .release = _release,
    .i2c = &_i2c_ops,
    .spi = &_spi_ops,
    .accepts_drive = false,
    .neopixel = _neopixel,
};

#else

// Other systems have no such kernel interface: the board is listed and refuses to start.

static bool _present(void) {
    return false;
}

static bool _open(void) {
    fprintf(stderr, "circuitpython: --board gpiochip runs on Linux only (a Raspberry Pi and the like)\n");
    return false;
}

static void _close(void) {
}

static void _reset(uint32_t keep_mask) {
    (void)keep_mask;
}

static digitalinout_result_t _configure(uint8_t pin, bool output, bool open_drain, digitalio_pull_t pull) {
    (void)pin;
    (void)output;
    (void)open_drain;
    (void)pull;
    return DIGITALINOUT_PIN_BUSY;
}

static bool _write(uint8_t pin, bool value) {
    (void)pin;
    (void)value;
    return false;
}

static bool _read(uint8_t pin, bool *level) {
    (void)pin;
    *level = false;
    return false;
}

const desktop_pin_backend_t gpiochip_backend = {
    .name = "GPIO",
    .present = _present,
    .open = _open,
    .close = _close,
    .reset = _reset,
    .configure = _configure,
    .write = _write,
    .read = _read,
    .lost = NULL,
    .pin_ok = NULL,
    .i2c = NULL,
    .spi = NULL,
    .accepts_drive = false,
};

#endif
