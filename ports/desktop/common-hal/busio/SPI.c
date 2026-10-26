// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// busio.SPI over the pin backend's SPI ops: the FT232H's MPSSE (mpsse_spi.c) or a Linux spidev
// (gpiochip.c); each names its SCK, MOSI and MISO pins.

#include "py/mpconfig.h"

#if CIRCUITPY_BUSIO_SPI

#include <string.h>

#include "py/runtime.h"
#include "shared-bindings/busio/SPI.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "bus_trace.h"
#include "desktop_pins.h"

static const desktop_spi_ops_t *_ops(void) {
    const desktop_spi_ops_t *ops = desktop_pin_backend->spi;
    if (ops == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("busio.SPI needs --board ft232h or gpiochip"));
    }
    return ops;
}

// board.SPI() passes placeholders for the board's own bus pins, which nobody may be using.
static const mcu_pin_obj_t *_bus_pin(const mcu_pin_obj_t *pin) {
    if (pin == NULL || pin->number < DESKTOP_PIN_COUNT) {
        return pin;
    }
    const mcu_pin_obj_t *real = desktop_bus_pin(pin);
    assert_pin_free(real);
    return real;
}

void common_hal_busio_spi_construct(busio_spi_obj_t *self,
    const mcu_pin_obj_t *clock, const mcu_pin_obj_t *mosi,
    const mcu_pin_obj_t *miso, bool half_duplex) {
    const desktop_spi_ops_t *ops = _ops();
    if (half_duplex) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("half duplex SPI"));
    }
    if (ops->sck == DESKTOP_BUS_ANY_PIN) {
        // The virtual board's virtual bus: any free pins, the placeholders too.
        assert_pin_free(clock);
        if (mosi != NULL) {
            assert_pin_free(mosi);
        }
        if (miso != NULL) {
            assert_pin_free(miso);
        }
    } else {
        clock = _bus_pin(clock);
        mosi = _bus_pin(mosi);
        miso = _bus_pin(miso);
        if (clock->number != ops->sck || (mosi != NULL && mosi->number != ops->mosi) || (miso != NULL && miso->number != ops->miso)) {
            raise_ValueError_invalid_pins();
        }
    }
    desktop_pin_backend_raise_if_lost();
    // Mode 0 at 250 kHz until configure(): a device may keep what the last program set.
    uint32_t actual = 250000;
    if (!ops->construct(mosi != NULL, miso != NULL) || ops->configure(250000, 0, 0, 8, &actual) != DESKTOP_SPI_OK) {
        desktop_pin_backend_raise_if_lost();
        desktop_raise_bus_error();
    }
    self->clock = clock;
    self->mosi = mosi;
    self->miso = miso;
    self->has_lock = false;
    self->polarity = 0;
    self->phase = 0;
    self->frequency = actual;
    common_hal_mcu_pin_claim(clock);
    if (mosi != NULL) {
        common_hal_mcu_pin_claim(mosi);
    }
    if (miso != NULL) {
        common_hal_mcu_pin_claim(miso);
    }
}

bool common_hal_busio_spi_deinited(busio_spi_obj_t *self) {
    return self->clock == NULL;
}

void common_hal_busio_spi_deinit(busio_spi_obj_t *self) {
    if (common_hal_busio_spi_deinited(self)) {
        return;
    }
    if (desktop_pin_backend->spi != NULL) {
        desktop_pin_backend->spi->deinit();
    }
    common_hal_reset_pin(self->clock);
    common_hal_reset_pin(self->mosi);
    common_hal_reset_pin(self->miso);
    self->clock = NULL;
    self->mosi = NULL;
    self->miso = NULL;
}

void common_hal_busio_spi_mark_deinit(busio_spi_obj_t *self) {
    self->clock = NULL;
}

#if CIRCUITPY_BULK_RESET
void common_hal_busio_spi_never_reset(busio_spi_obj_t *self) {
    common_hal_never_reset_pin(self->clock);
    if (self->mosi != NULL) {
        common_hal_never_reset_pin(self->mosi);
    }
    if (self->miso != NULL) {
        common_hal_never_reset_pin(self->miso);
    }
}
#endif

bool common_hal_busio_spi_configure(busio_spi_obj_t *self, uint32_t baudrate, uint8_t polarity, uint8_t phase, uint8_t bits) {
    desktop_pin_backend_raise_if_lost();
    uint32_t actual = baudrate;
    desktop_spi_result_t result = desktop_pin_backend->spi->configure(baudrate, polarity, phase, bits, &actual);
    desktop_pin_backend_raise_if_lost();
    if (result == DESKTOP_SPI_BAD_BITS) {
        return false;
    }
    if (result == DESKTOP_SPI_BAD_MODE) {
        mp_raise_ValueError(MP_ERROR_TEXT("SPI modes 1 and 3 are not supported"));
    }
    if (result == DESKTOP_SPI_FAILED) {
        mp_raise_OSError(MP_EIO);
    }
    self->frequency = actual;
    self->polarity = polarity;
    self->phase = phase;
    bus_trace_spi_config(self->frequency, polarity, phase, bits);
    return true;
}

bool common_hal_busio_spi_try_lock(busio_spi_obj_t *self) {
    if (self->has_lock) {
        return false;
    }
    self->has_lock = true;
    return true;
}

bool common_hal_busio_spi_has_lock(busio_spi_obj_t *self) {
    return self->has_lock;
}

void common_hal_busio_spi_unlock(busio_spi_obj_t *self) {
    self->has_lock = false;
}

static bool _transfer(const uint8_t *data_out, uint8_t fill, uint8_t *data_in, size_t len) {
    desktop_pin_backend_raise_if_lost();
    bool ok = desktop_pin_backend->spi->transfer(data_out, fill, data_in, len);
    desktop_pin_backend_raise_if_lost();
    return ok;
}

bool common_hal_busio_spi_write(busio_spi_obj_t *self, const uint8_t *data, size_t len) {
    bool ok = _transfer(data, 0, NULL, len);
    bus_trace_spi(data, NULL, len, ok);
    return ok;
}

bool common_hal_busio_spi_read(busio_spi_obj_t *self, uint8_t *data, size_t len, uint8_t write_value) {
    bool ok = _transfer(NULL, write_value, data, len);
    bus_trace_spi(NULL, data, len, ok);
    return ok;
}

bool common_hal_busio_spi_transfer(busio_spi_obj_t *self, const uint8_t *data_out, uint8_t *data_in, size_t len) {
    bool ok = _transfer(data_out, 0, data_in, len);
    bus_trace_spi(data_out, data_in, len, ok);
    return ok;
}

uint32_t common_hal_busio_spi_get_frequency(busio_spi_obj_t *self) {
    return self->frequency;
}

uint8_t common_hal_busio_spi_get_phase(busio_spi_obj_t *self) {
    return self->phase;
}

uint8_t common_hal_busio_spi_get_polarity(busio_spi_obj_t *self) {
    return self->polarity;
}

#endif
