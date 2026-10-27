// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// Devices a program drives that no board file names: on the virtual board's virtual buses a
// fourwire display on any cs pin gets a colour TFT of the size the program gives, an I2C display
// bus at any address an SSD1306, a driver's I2CDevice the modelled sensor at its address, as a
// program written for the module finds it on a board.

#include <stdio.h>

#include "py/obj.h"
#include "shared-bindings/digitalio/DigitalInOut.h"
#include "shared-bindings/fourwire/FourWire.h"
#include "shared-bindings/i2cdisplaybus/I2CDisplayBus.h"

#include "i2c_virtual.h"
#include "desktop_pins.h"
#include "spi_virtual.h"
#include "vdisplay.h"

static int _pin_of(mp_obj_t pin_io) {
    if (!mp_obj_is_type(pin_io, &digitalio_digitalinout_type)) {
        return -1;
    }
    const digitalio_digitalinout_obj_t *io = MP_OBJ_TO_PTR(pin_io);
    return io->pin != NULL ? io->pin->number : -1;
}

// The first of NAME, NAME2, NAME3... no display has yet.
static void _free_name(const char *base, char *name, size_t len) {
    snprintf(name, len, "%s", base);
    for (unsigned i = 2; vdisplay_by_name(name) != NULL && i < 10; i++) {
        snprintf(name, len, "%s%c", base, '0' + i);
    }
}

void i2cdevice_port_attach(mp_obj_t i2c, uint8_t device_address) {
    (void)i2c;
    if (desktop_pin_backend == &desktop_virtual_backend) {
        i2c_virtual_attach_default(device_address);
    }
}

void i2cdisplaybus_port_attach(busio_i2c_obj_t *i2c, uint16_t device_address) {
    (void)i2c;
    if (desktop_pin_backend != &desktop_virtual_backend || device_address > 0x77 || i2c_virtual_slot((uint8_t)device_address) != NULL) {
        return;
    }
    char name[12], spec[32];
    const char *why;
    _free_name("OLED", name, sizeof(name));
    snprintf(spec, sizeof(spec), "ssd1306 128x64 0x%02x", device_address);
    i2c_virtual_add(name, spec, &why);
}

void busdisplay_port_attach(mp_obj_t bus, uint16_t width, uint16_t height, uint16_t rotation) {
    if (desktop_pin_backend != &desktop_virtual_backend) {
        return;
    }
    if (mp_obj_is_type(bus, &i2cdisplaybus_i2cdisplaybus_type)) {
        // An SSD1306 as large as the driver says: 128x32 and the like.
        const i2cdisplaybus_i2cdisplaybus_obj_t *b = MP_OBJ_TO_PTR(bus);
        i2c_virtual_device_t *dev = i2c_virtual_at((uint8_t)b->address);
        if (dev != NULL && dev->display != NULL && width <= 128 && height <= 64 && height % 8 == 0) {
            vdisplay_resize(dev->display, width, height);
        }
        return;
    }
    if (!mp_obj_is_type(bus, &fourwire_fourwire_type)) {
        return;
    }
    const fourwire_fourwire_obj_t *b = MP_OBJ_TO_PTR(bus);
    int cs = _pin_of(b->chip_select), dc = _pin_of(b->command);
    if (cs < 0 || dc < 0) {
        return;
    }
    vdisplay_t *display = spi_virtual_display(cs);
    if (display != NULL) {
        vdisplay_resize(display, width, height);
    } else {
        char name[12], spec[32];
        const char *why;
        _free_name("TFT", name, sizeof(name));
        snprintf(spec, sizeof(spec), "st7789 %ux%u", (unsigned)width, (unsigned)height);
        spi_virtual_add(name, spec, cs, dc, &why);
    }
    // displayio turns the picture in software: the panel turns it back, upright as on the module.
    spi_virtual_set_rotation(cs, rotation);
}
