// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// busio.I2C over the pin backend's I2C ops: the FT232H's MPSSE (mpsse_i2c.c) or the MCP2221's
// engine (mcp2221.c), or a Linux i2c-dev bus (gpiochip.c); each names its SCL and SDA pins.

#include "py/mpconfig.h"

#if CIRCUITPY_BUSIO_I2C

#include "py/mperrno.h"
#include "py/runtime.h"
#include "shared-bindings/busio/I2C.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "bus_trace.h"
#include "desktop_pins.h"
#include "mpsse.h"

static const desktop_i2c_ops_t *_ops(void) {
    const desktop_i2c_ops_t *ops = desktop_pin_backend->i2c;
    if (ops == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("busio.I2C needs --board ft232h, mcp2221 or gpiochip"));
    }
    return ops;
}

static bool _with_d2(void) {
    // The FT232H reads SDA back on D2, so that pin goes with the bus.
    return desktop_pin_backend == &desktop_ft232h_backend;
}

void common_hal_busio_i2c_construct(busio_i2c_obj_t *self,
    const mcu_pin_obj_t *scl, const mcu_pin_obj_t *sda, uint32_t frequency, uint32_t timeout_ms) {
    (void)timeout_ms;
    const desktop_i2c_ops_t *ops = _ops();
    if (ops->scl == DESKTOP_BUS_ANY_PIN) {
        // The virtual board's virtual bus: any two free pins, the placeholders too.
        assert_pin_free(scl);
        assert_pin_free(sda);
    } else {
        if (scl->number >= DESKTOP_PIN_COUNT || sda->number >= DESKTOP_PIN_COUNT) {
            // board.I2C(): the board's own bus pins, which nobody may be using.
            scl = desktop_bus_pin(scl);
            sda = desktop_bus_pin(sda);
            assert_pin_free(scl);
            assert_pin_free(sda);
        }
        if (scl->number != ops->scl || sda->number != ops->sda) {
            raise_ValueError_invalid_pins();
        }
    }
    if (_with_d2() && !common_hal_mcu_pin_is_free(&pin_GPIO2)) {
        raise_ValueError_invalid_pins();
    }
    desktop_pin_backend_raise_if_lost();
    if (!ops->construct(frequency, &self->frequency)) {
        desktop_pin_backend_raise_if_lost();
        desktop_raise_bus_error();
    }
    self->scl = scl;
    self->sda = sda;
    self->has_lock = false;
    common_hal_mcu_pin_claim(scl);
    common_hal_mcu_pin_claim(sda);
    if (_with_d2()) {
        common_hal_mcu_pin_claim(&pin_GPIO2);
    }
}

bool common_hal_busio_i2c_deinited(busio_i2c_obj_t *self) {
    return self->scl == NULL;
}

void common_hal_busio_i2c_deinit(busio_i2c_obj_t *self) {
    if (common_hal_busio_i2c_deinited(self)) {
        return;
    }
    desktop_pin_backend->i2c->deinit();
    common_hal_reset_pin(self->scl);
    common_hal_reset_pin(self->sda);
    if (_with_d2()) {
        common_hal_reset_pin(&pin_GPIO2);
    }
    self->scl = NULL;
    self->sda = NULL;
}

void common_hal_busio_i2c_mark_deinit(busio_i2c_obj_t *self) {
    self->scl = NULL;
}

#if CIRCUITPY_BULK_RESET
void common_hal_busio_i2c_never_reset(busio_i2c_obj_t *self) {
    common_hal_never_reset_pin(self->scl);
    common_hal_never_reset_pin(self->sda);
    if (_with_d2()) {
        common_hal_never_reset_pin(&pin_GPIO2);
    }
}
#endif

bool common_hal_busio_i2c_try_lock(busio_i2c_obj_t *self) {
    if (self->has_lock) {
        return false;
    }
    self->has_lock = true;
    return true;
}

bool common_hal_busio_i2c_has_lock(busio_i2c_obj_t *self) {
    return self->has_lock;
}

void common_hal_busio_i2c_unlock(busio_i2c_obj_t *self) {
    self->has_lock = false;
}

bool common_hal_busio_i2c_probe(busio_i2c_obj_t *self, uint8_t addr) {
    (void)self;
    desktop_pin_backend_raise_if_lost();
    int status = desktop_pin_backend->i2c->write(addr, NULL, 0, true);
    bus_trace_i2c(addr, 'P', NULL, 0, true, status);
    desktop_pin_backend_raise_if_lost();
    return status == 0;
}

mp_negative_errno_t common_hal_busio_i2c_write(busio_i2c_obj_t *self, uint16_t address, const uint8_t *data, size_t len) {
    (void)self;
    desktop_pin_backend_raise_if_lost();
    int status = desktop_pin_backend->i2c->write(address, data, len, true);
    bus_trace_i2c(address, 'W', data, len, true, status);
    desktop_pin_backend_raise_if_lost();
    return status;
}

mp_negative_errno_t common_hal_busio_i2c_read(busio_i2c_obj_t *self, uint16_t address, uint8_t *data, size_t len) {
    (void)self;
    desktop_pin_backend_raise_if_lost();
    int status = desktop_pin_backend->i2c->read(address, data, len);
    bus_trace_i2c(address, 'R', data, len, true, status);
    desktop_pin_backend_raise_if_lost();
    return status;
}

mp_negative_errno_t common_hal_busio_i2c_write_read(busio_i2c_obj_t *self, uint16_t address,
    uint8_t *out_data, size_t out_len, uint8_t *in_data, size_t in_len) {
    (void)self;
    desktop_pin_backend_raise_if_lost();
    int status = desktop_pin_backend->i2c->write(address, out_data, out_len, false);
    bus_trace_i2c(address, 'W', out_data, out_len, false, status);
    if (status == 0) {
        status = desktop_pin_backend->i2c->read(address, in_data, in_len);
        bus_trace_i2c(address, 'R', in_data, in_len, true, status);
    }
    desktop_pin_backend_raise_if_lost();
    return status;
}

#endif
