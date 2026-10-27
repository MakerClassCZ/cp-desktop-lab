// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// libusb-1.0 by dlopen / LoadLibrary with the handful of prototypes used, so the build needs no
// header or import library and the program starts without the library.

#include "usb_host.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "desktop_port.h"

#if defined(_WIN32) && !defined(_WIN64)
#define USB_CALL __stdcall
#else
#define USB_CALL
#endif

typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;

// The first 18 bytes of libusb's struct, as the library lays them out.
struct libusb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
};

static struct {
    int(USB_CALL * init)(libusb_context * *);
    void(USB_CALL * exit)(libusb_context *);
    ptrdiff_t(USB_CALL * get_device_list)(libusb_context *, libusb_device ***);
    void(USB_CALL * free_device_list)(libusb_device * *, int);
    int(USB_CALL * get_device_descriptor)(libusb_device *, struct libusb_device_descriptor *);
    int(USB_CALL * open)(libusb_device *, libusb_device_handle **);
    void(USB_CALL * close)(libusb_device_handle *);
    int(USB_CALL * get_configuration)(libusb_device_handle *, int *);
    int(USB_CALL * set_configuration)(libusb_device_handle *, int);
    int(USB_CALL * set_auto_detach_kernel_driver)(libusb_device_handle *, int);
    int(USB_CALL * claim_interface)(libusb_device_handle *, int);
    int(USB_CALL * release_interface)(libusb_device_handle *, int);
    int(USB_CALL * control_transfer)(libusb_device_handle *, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char *, uint16_t, unsigned);
    int(USB_CALL * bulk_transfer)(libusb_device_handle *, unsigned char, unsigned char *, int, int *, unsigned);
    const char *(USB_CALL *strerror)(int);
    libusb_device *(USB_CALL *get_device)(libusb_device_handle *);
    int(USB_CALL * get_max_packet_size)(libusb_device *, unsigned char);
} _lib;

static libusb_context *_ctx;
static libusb_device_handle *_handle;
static bool _claimed;

#ifdef _WIN32
static HMODULE _module;
static void *_sym(const char *name) {
    return (void *)GetProcAddress(_module, name);
}
#else
static void *_module;
static void *_sym(const char *name) {
    return dlsym(_module, name);
}
#endif

bool usb_host_load(char *err, size_t err_len) {
    if (_module != NULL) {
        return true;
    }
    #ifdef _WIN32
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\libusb-1.0.dll", desktop_options.exe_dir);
    _module = LoadLibraryA(path);
    if (_module == NULL) {
        _module = LoadLibraryA("libusb-1.0.dll");
    }
    if (_module == NULL) {
        snprintf(err, err_len, "libusb-1.0 not found: put libusb-1.0.dll beside the program (error %lu)", (unsigned long)GetLastError());
        return false;
    }
    #else
    #ifdef __APPLE__
    // Beside the program, else Homebrew's (Apple silicon, Intel), which dlopen does not search.
    static const char *const names[] = { "@executable_path/libusb-1.0.0.dylib",
                                         "/opt/homebrew/lib/libusb-1.0.0.dylib", "/usr/local/lib/libusb-1.0.0.dylib" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && _module == NULL; i++) {
        _module = dlopen(names[i], RTLD_NOW);
    }
    if (_module == NULL) {
        snprintf(err, err_len, "libusb-1.0 not found: brew install libusb");
        return false;
    }
    #else
    _module = dlopen("libusb-1.0.so.0", RTLD_NOW);
    if (_module == NULL) {
        snprintf(err, err_len, "libusb-1.0 not found: %s", dlerror());
        return false;
    }
    #endif
    #endif
    #define LOAD(field, name) \
    if ((*(void **)&_lib.field = _sym(name)) == NULL) { \
        snprintf(err, err_len, "libusb-1.0 not found: no %s in the library", name); \
        return false; \
    }
    LOAD(init, "libusb_init");
    LOAD(exit, "libusb_exit");
    LOAD(get_device_list, "libusb_get_device_list");
    LOAD(free_device_list, "libusb_free_device_list");
    LOAD(get_device_descriptor, "libusb_get_device_descriptor");
    LOAD(open, "libusb_open");
    LOAD(close, "libusb_close");
    LOAD(get_configuration, "libusb_get_configuration");
    LOAD(set_configuration, "libusb_set_configuration");
    LOAD(set_auto_detach_kernel_driver, "libusb_set_auto_detach_kernel_driver");
    LOAD(claim_interface, "libusb_claim_interface");
    LOAD(release_interface, "libusb_release_interface");
    LOAD(control_transfer, "libusb_control_transfer");
    LOAD(bulk_transfer, "libusb_bulk_transfer");
    LOAD(strerror, "libusb_strerror");
    LOAD(get_device, "libusb_get_device");
    LOAD(get_max_packet_size, "libusb_get_max_packet_size");
#undef LOAD
    return true;
}

const char *usb_host_strerror(int code) {
    return _lib.strerror != NULL ? _lib.strerror(code) : "libusb not loaded";
}

bool usb_host_present(uint16_t vid, uint16_t pid) {
    char err[64];
    libusb_context *ctx = NULL;
    if (!usb_host_load(err, sizeof(err)) || _lib.init(&ctx) < 0) {
        return false;
    }
    libusb_device **list = NULL;
    ptrdiff_t count = _lib.get_device_list(ctx, &list);
    bool found = false;
    for (ptrdiff_t i = 0; i < count && !found; i++) {
        struct libusb_device_descriptor desc;
        found = _lib.get_device_descriptor(list[i], &desc) == 0 && desc.idVendor == vid && desc.idProduct == pid;
    }
    if (count >= 0) {
        _lib.free_device_list(list, 1);
    }
    _lib.exit(ctx);
    return found;
}

bool usb_host_open(uint16_t vid, uint16_t pid, char *err, size_t err_len) {
    if (_handle != NULL) {
        return true;
    }
    int status = _lib.init(&_ctx);
    if (status < 0) {
        snprintf(err, err_len, "libusb: %s", usb_host_strerror(status));
        return false;
    }
    libusb_device **list = NULL;
    ptrdiff_t count = _lib.get_device_list(_ctx, &list);
    libusb_device *found = NULL;
    for (ptrdiff_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        if (_lib.get_device_descriptor(list[i], &desc) == 0 && desc.idVendor == vid && desc.idProduct == pid) {
            found = list[i];
            break;
        }
    }
    if (found == NULL) {
        snprintf(err, err_len, "no FT232H found (VID %04x PID %04x)", vid, pid);
    } else {
        status = _lib.open(found, &_handle);
        if (status < 0) {
            _handle = NULL;
            snprintf(err, err_len, "cannot open FT232H: %s", usb_host_strerror(status));
        }
    }
    if (count >= 0) {
        _lib.free_device_list(list, 1);
    }
    if (_handle == NULL) {
        _lib.exit(_ctx);
        _ctx = NULL;
        return false;
    }
    _lib.set_auto_detach_kernel_driver(_handle, 1);
    int config = 0;
    if (_lib.get_configuration(_handle, &config) == 0 && config == 0) {
        _lib.set_configuration(_handle, 1);
    }
    status = _lib.claim_interface(_handle, 0);
    if (status < 0) {
        snprintf(err, err_len, "cannot open FT232H: %s", usb_host_strerror(status));
        usb_host_close();
        return false;
    }
    _claimed = true;
    return true;
}

void usb_host_close(void) {
    if (_handle != NULL) {
        if (_claimed) {
            _lib.release_interface(_handle, 0);
            _claimed = false;
        }
        _lib.close(_handle);
        _handle = NULL;
    }
    if (_ctx != NULL) {
        _lib.exit(_ctx);
        _ctx = NULL;
    }
}

int usb_host_max_packet(uint8_t endpoint) {
    if (_handle == NULL) {
        return 0;
    }
    int size = _lib.get_max_packet_size(_lib.get_device(_handle), endpoint);
    return size > 0 ? size : 0;
}

int usb_host_control(uint8_t request, uint16_t value, uint16_t index) {
    return _lib.control_transfer(_handle, 0x40, request, value, index, NULL, 0, 1000);
}

int usb_host_bulk_out(uint8_t endpoint, const uint8_t *data, size_t len, unsigned timeout_ms) {
    int moved = 0;
    int status = _lib.bulk_transfer(_handle, endpoint, (unsigned char *)data, (int)len, &moved, timeout_ms);
    if (status < 0 && !(status == USB_HOST_ERROR_TIMEOUT && moved > 0)) {
        return status;
    }
    return moved;
}

int usb_host_bulk_in(uint8_t endpoint, uint8_t *buf, size_t len, unsigned timeout_ms) {
    int moved = 0;
    int status = _lib.bulk_transfer(_handle, endpoint, buf, (int)len, &moved, timeout_ms);
    if (status < 0 && !(status == USB_HOST_ERROR_TIMEOUT && moved > 0)) {
        return status;
    }
    return moved;
}
