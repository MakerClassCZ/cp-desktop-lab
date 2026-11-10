// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "i2c_virtual.h"

#include "fault.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "py/mperrno.h"

#include "bus_trace.h"
#include "desktop_port.h"
#include "fault.h"
#include "parts.h"
#include "touch.h"
#include "i2c/models.h"

#define DEVICES_MAX 12

static i2c_virtual_device_t _devices[DEVICES_MAX];
static int _count;

// The models, one file each in i2c/; none with CIRCUITPY_VIRTUAL_DEVICES = 0.
static const i2c_virtual_model_t *const _models[] = {
    #if CIRCUITPY_VIRTUAL_DEVICES
    // A driver's scan attaches the first model with the address: LIS3DH before MCP9808 (0x18).
    &i2c_model_lis3dh, &i2c_model_bme280, &i2c_model_bmp280, &i2c_model_mcp9808, &i2c_model_ds3231,
    &i2c_model_lc709203f, &i2c_model_lsm6ds33, &i2c_model_lis3mdl, &i2c_model_apds9960, &i2c_model_sht31d,
    &i2c_model_sht4x, &i2c_model_scd41, &i2c_model_scd40, &i2c_model_aht20, &i2c_model_max17048, &i2c_model_drv2605,
    &i2c_model_pca9685, &i2c_model_ht16k33, &i2c_model_is31fl3741, &i2c_model_tsc2007,
    &i2c_model_ft6206, &i2c_model_cst816,
    // A seesaw library at an address no model has gets the rotary encoder, a seesaw like most.
    &i2c_model_rotary, &i2c_model_soil, &i2c_model_crickit, &i2c_model_ssd1306,
    #endif
    NULL
};

const i2c_virtual_model_t *i2c_virtual_model(const char *name) {
    for (size_t i = 0; _models[i] != NULL; i++) {
        if (strcasecmp(name, _models[i]->name) == 0) {
            return _models[i];
        }
    }
    return NULL;
}

i2c_virtual_device_t *i2c_virtual_device(int i) {
    return i >= 0 && i < _count ? &_devices[i] : NULL;
}

i2c_virtual_device_t *i2c_virtual_slot(uint16_t address) {
    for (int i = 0; i < _count; i++) {
        if (_devices[i].address == address) {
            return &_devices[i];
        }
    }
    return NULL;
}

i2c_virtual_device_t *i2c_virtual_at(uint16_t address) {
    i2c_virtual_device_t *dev = i2c_virtual_slot(address);
    return dev != NULL && dev->present ? dev : NULL;
}

i2c_virtual_device_t *i2c_virtual_by_name(const char *name) {
    for (int i = 0; i < _count; i++) {
        if (_devices[i].present && strcasecmp(_devices[i].name, name) == 0) {
            return &_devices[i];
        }
    }
    return NULL;
}

void i2c_virtual_detach(i2c_virtual_device_t *dev) {
    if (dev->present) {
        dev->present = false;
        parts_detach_device(dev);
    }
}

// Whether the module (adafruit_lsm6ds.lsm6ds33) is the library or one of its submodules.
static bool _drives(const i2c_virtual_model_t *m, const char *module) {
    size_t n = m->driver != NULL ? strlen(m->driver) : 0;
    return n > 0 && module != NULL && strncmp(module, m->driver, n) == 0 && (module[n] == '\0' || module[n] == '.');
}

bool i2c_virtual_attach_default(uint8_t address, const char *driver) {
    if (i2c_virtual_slot(address) != NULL) {
        return false;
    }
    // The driver's chip at its own address, else at the one its address pins give it; a driver
    // of no model's chip gets the first model whose address it is.
    const i2c_virtual_model_t *m = NULL;
    for (int pass = 0; pass < 3 && m == NULL; pass++) {
        for (size_t i = 0; _models[i] != NULL && m == NULL; i++) {
            const i2c_virtual_model_t *c = _models[i];
            bool here = c->address == address, drives = _drives(c, driver);
            if (c->attach == NULL && (pass == 0 ? here && drives : pass == 1 ? drives : here)) {
                m = c;
            }
        }
    }
    if (m == NULL) {
        return false;
    }
    char name[12];
    const char *why;
    size_t k = 0;
    for (; m->name[k] != '\0' && k < sizeof(name) - 1; k++) {
        name[k] = (char)toupper((unsigned char)m->name[k]);
    }
    name[k] = '\0';
    for (int n = 2; i2c_virtual_by_name(name) != NULL && n < 10; n++) {
        snprintf(name + k, sizeof(name) - k, "_%d", n);
    }
    char spec[32];
    snprintf(spec, sizeof(spec), "%s 0x%02x", m->name, (unsigned)address);
    if (!i2c_virtual_add(name, spec, &why)) {
        return false;
    }
    i2c_virtual_slot(address)->automatic = true;
    // The library that chose it, and the other models at the address for a driver of another chip.
    char said[160] = "";
    size_t used = 0;
    if (_drives(m, driver)) {
        used = (size_t)snprintf(said, sizeof(said), ", for %s", m->driver);
    }
    const char *sep = "; for another chip: ";
    for (size_t j = 0; _models[j] != NULL && used < sizeof(said); j++) {
        if (_models[j] != m && _models[j]->address == address && _models[j]->attach == NULL) {
            used += (size_t)snprintf(said + used, sizeof(said) - used, "%s[i2c] X = \"%s\"", sep, _models[j]->name);
            sep = " or ";
        }
    }
    fprintf(stderr, "virtual: %s attached at I2C 0x%02x (automatic%s)\r\n", name, (unsigned)address, said);
    return true;
}

bool i2c_virtual_add(const char *name, const char *spec, const char **why) {
    static char message[256];
    char model_name[16] = "", words[64];
    unsigned address = 0, width = 0, height = 0;
    bool addressed = false;
    snprintf(words, sizeof(words), "%s", spec);
    char display[16] = "";
    // MODEL, then a size (a display's) and an address, in either order; a touch controller's
    // display=NAME, swap, flip_x and flip_y.
    for (char *word = strtok(words, " "); word != NULL; word = strtok(NULL, " ")) {
        if (model_name[0] == '\0') {
            snprintf(model_name, sizeof(model_name), "%s", word);
        } else if (strncmp(word, "display=", 8) == 0) {
            snprintf(display, sizeof(display), "%s", word + 8);
        } else if (strcmp(word, "swap") == 0 || strncmp(word, "flip_", 5) == 0) {
            continue;
        } else if (strncasecmp(word, "0x", 2) != 0 && sscanf(word, "%ux%u", &width, &height) == 2) {
            continue;
        } else {
            address = (unsigned)strtoul(word, NULL, 0);
            addressed = true;
        }
    }
    const i2c_virtual_model_t *model = NULL;
    int n = snprintf(message, sizeof(message), "unknown I2C device %s (", model_name);
    for (size_t i = 0; _models[i] != NULL; i++) {
        if (strcasecmp(model_name, _models[i]->name) == 0) {
            model = _models[i];
        }
        if (n < (int)sizeof(message)) {
            const char *sep = i == 0 ? "" : _models[i + 1] == NULL ? " or " : ", ";
            n += snprintf(message + n, sizeof(message) - n, "%s%s", sep, _models[i]->name);
        }
    }
    if (model == NULL) {
        if (n < (int)sizeof(message)) {
            snprintf(message + n, sizeof(message) - n, "%s)", _models[0] == NULL ? "none in this build" : "");
        }
        *why = message;
        return false;
    }
    if (!addressed) {
        address = model->address;
    }
    if (address < 0x08 || address > 0x77) {
        *why = "bad I2C address (0x08-0x77)";
        return false;
    }
    i2c_virtual_device_t *dev = i2c_virtual_slot(address);
    if ((dev == NULL && _count == DEVICES_MAX) || strlen(name) >= sizeof(_devices[0].name)) {
        *why = strlen(name) >= sizeof(_devices[0].name) ? "a name of 11 characters at most" : "more than 12 I2C devices";
        return false;
    }
    if (dev != NULL && dev->present && dev->model != model && dev->model->attach != NULL) {
        snprintf(message, sizeof(message), "address 0x%02x taken by the display %s", address, dev->name);
        *why = message;
        return false;
    }
    for (int i = 0; i < _count; i++) {
        i2c_virtual_device_t *other = &_devices[i];
        if (other == dev ? other->model != model : strcasecmp(other->name, name) == 0 && other->model->attach == NULL) {
            i2c_virtual_detach(other);
        }
    }
    if (dev != NULL && dev->model == model) {
        // The same model at the address: attached again with its values, under the new name.
        dev->present = true;
        dev->automatic = false;
        snprintf(dev->name, sizeof(dev->name), "%s", name);
        for (int c = 0; c < model->channel_count; c++) {
            parts_add_sensor(dev, c);
        }
        if (model->touch != I2C_TOUCH_NONE) {
            touch_bind_i2c(dev, display, spec);
        }
        return true;
    }
    bool fresh = dev == NULL;
    if (fresh) {
        dev = &_devices[_count];
    }
    memset(dev, 0, sizeof(*dev));
    dev->model = model;
    dev->address = (uint8_t)address;
    snprintf(dev->name, sizeof(dev->name), "%s", name);
    if (model->attach != NULL && !model->attach(dev, width, height, why)) {
        return false;
    }
    if (fresh) {
        _count++;
    }
    for (int c = 0; c < model->channel_count; c++) {
        dev->value[c] = model->channels[c].initial;
        if (parts_add_sensor(dev, c) == NULL) {
            *why = "too many parts";
            return false;
        }
    }
    if (model->touch != I2C_TOUCH_NONE) {
        touch_bind_i2c(dev, display, spec);
    }
    dev->present = true;
    return true;
}

void i2c_virtual_set(i2c_virtual_device_t *dev, int channel, double value) {
    const i2c_virtual_channel_t *ch = &dev->model->channels[channel];
    value = value < ch->min ? ch->min : value > ch->max ? ch->max : value;
    dev->value[channel] = value;
    bus_trace_text("PANEL %s.%s = %.*f %s", dev->name, ch->name, ch->decimals, value, ch->unit);
}

// ---- the bus ----

static bool _construct(uint32_t frequency, uint32_t *actual) {
    *actual = frequency;
    return true;
}

static void _deinit(void) {
}

// "fault i2c nack": the address is not acknowledged, as with no device there; "hold": SDA held
// low, every transfer times out.
static int _fault(uint16_t address) {
    fault_t f = fault_check("i2c", NULL, address);
    return !f.fired ? 0 : strcmp(f.action, "nack") == 0 ? -MP_ENODEV : -MP_ETIMEDOUT;
}

static int _write(uint16_t address, const uint8_t *data, size_t len, bool stop) {
    (void)stop;
    int fault = _fault(address);
    if (fault != 0) {
        return fault;
    }
    i2c_virtual_device_t *dev = i2c_virtual_at(address);
    if (dev == NULL) {
        return -MP_ENODEV;
    }
    if (len > 0) {
        dev->model->write(dev, data, len);
    }
    if (dev->nack) {
        dev->nack = false;
        return -MP_EIO;
    }
    return 0;
}

static int _read(uint16_t address, uint8_t *data, size_t len) {
    int fault = _fault(address);
    if (fault != 0) {
        return fault;
    }
    i2c_virtual_device_t *dev = i2c_virtual_at(address);
    if (dev == NULL) {
        return -MP_ENODEV;
    }
    dev->model->read(dev, data, len);
    return 0;
}

const desktop_i2c_ops_t i2c_virtual_ops = {
    .scl = DESKTOP_BUS_ANY_PIN,
    .sda = DESKTOP_BUS_ANY_PIN,
    .construct = _construct,
    .deinit = _deinit,
    .write = _write,
    .read = _read,
};
