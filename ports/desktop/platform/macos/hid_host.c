// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// The HID device through macOS's IOKit: reports out with IOHIDDeviceSetReport, the device's
// reports in on a dispatch queue of their own, handed to the VM thread through a semaphore.

#include "hid_host.h"

#include <IOKit/hid/IOHIDManager.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>

static char _last_error[128];

const char *hid_host_last_error(void) {
    return _last_error;
}

static IOHIDDeviceRef _device;
static dispatch_queue_t _queue;
static dispatch_semaphore_t _arrived;
static uint8_t _in[HID_REPORT_SIZE];         // where IOKit writes a report as it comes
static uint8_t _report[HID_REPORT_SIZE];     // the last whole report, for the VM thread
static volatile bool _waiting;

static void _input(void *context, IOReturn result, void *sender, IOHIDReportType type,
    uint32_t id, uint8_t *report, CFIndex length) {
    (void)context;
    (void)sender;
    (void)type;
    (void)id;
    // Only an answer the VM thread waits for counts; a stray report does not stand in for the next.
    if (result == kIOReturnSuccess && _waiting) {
        memset(_report, 0, sizeof(_report));
        memcpy(_report, report, length < HID_REPORT_SIZE ? (size_t)length : HID_REPORT_SIZE);
        _waiting = false;
        dispatch_semaphore_signal(_arrived);
    }
}

// The devices plugged in with this VID and PID, or NULL; the caller releases the set.
static CFSetRef _devices(IOHIDManagerRef manager, uint16_t vid, uint16_t pid) {
    int v = vid, p = pid;
    CFNumberRef vendor = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFNumberRef product = CFNumberCreate(NULL, kCFNumberIntType, &p);
    const void *keys[] = { CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey) };
    const void *values[] = { vendor, product };
    CFDictionaryRef match = CFDictionaryCreate(NULL, keys, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(manager, match);
    CFRelease(match);
    CFRelease(product);
    CFRelease(vendor);
    CFSetRef set = IOHIDManagerCopyDevices(manager);
    if (set != NULL && CFSetGetCount(set) == 0) {
        CFRelease(set);
        set = NULL;
    }
    return set;
}

bool hid_host_open(uint16_t vid, uint16_t pid, const char *what, char *err, size_t err_len) {
    if (_device != NULL) {
        return true;
    }
    IOHIDManagerRef manager = IOHIDManagerCreate(NULL, kIOHIDOptionsTypeNone);
    CFSetRef set = _devices(manager, vid, pid);
    if (set == NULL) {
        CFRelease(manager);
        snprintf(err, err_len, "no %s found (VID %04x PID %04x)", what, vid, pid);
        return false;
    }
    CFIndex count = CFSetGetCount(set);
    const void *devices[count];
    CFSetGetValues(set, devices);
    IOReturn opened = kIOReturnNotFound;
    for (CFIndex i = 0; i < count && _device == NULL; i++) {
        IOHIDDeviceRef device = (IOHIDDeviceRef)devices[i];
        // Seized, so a second CircuitPython cannot have the same device.
        opened = IOHIDDeviceOpen(device, kIOHIDOptionsTypeSeizeDevice);
        if (opened == kIOReturnSuccess) {
            _device = (IOHIDDeviceRef)CFRetain(device);
        }
    }
    CFRelease(set);
    CFRelease(manager);
    if (_device == NULL) {
        snprintf(err, err_len, "cannot open %s: IOKit error 0x%x%s", what, opened,
            opened == kIOReturnExclusiveAccess ? " (another program has it)" : "");
        return false;
    }
    _queue = dispatch_queue_create("circuitpython.hid", NULL);
    _arrived = dispatch_semaphore_create(0);
    IOHIDDeviceRegisterInputReportCallback(_device, _in, sizeof(_in), _input, NULL);
    IOHIDDeviceSetDispatchQueue(_device, _queue);
    IOHIDDeviceActivate(_device);
    return true;
}

bool hid_host_present(uint16_t vid, uint16_t pid) {
    IOHIDManagerRef manager = IOHIDManagerCreate(NULL, kIOHIDOptionsTypeNone);
    CFSetRef set = _devices(manager, vid, pid);
    if (set != NULL) {
        CFRelease(set);
    }
    CFRelease(manager);
    return set != NULL;
}

void hid_host_close(void) {
    if (_device == NULL) {
        return;
    }
    IOHIDDeviceCancel(_device);
    // The queue runs nothing more for the device once the cancellation is through it.
    dispatch_sync(_queue, ^{
    });
    IOHIDDeviceClose(_device, kIOHIDOptionsTypeNone);
    CFRelease(_device);
    _device = NULL;
    dispatch_release(_arrived);
    dispatch_release(_queue);
}

bool hid_host_send(const uint8_t cmd[HID_REPORT_SIZE]) {
    if (_device == NULL) {
        return false;
    }
    IOReturn r = IOHIDDeviceSetReport(_device, kIOHIDReportTypeOutput, 0, cmd, HID_REPORT_SIZE);
    if (r != kIOReturnSuccess) {
        snprintf(_last_error, sizeof(_last_error), "HID write: IOKit error 0x%x", r);
        return false;
    }
    return true;
}

bool hid_host_exchange(const uint8_t cmd[HID_REPORT_SIZE], uint8_t reply[HID_REPORT_SIZE], unsigned timeout_ms) {
    while (dispatch_semaphore_wait(_arrived, DISPATCH_TIME_NOW) == 0) {
        // an answer that came after an earlier exchange gave up
    }
    _waiting = true;
    if (!hid_host_send(cmd)) {
        _waiting = false;
        return false;
    }
    if (dispatch_semaphore_wait(_arrived, dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ms * 1000000)) != 0) {
        _waiting = false;
        snprintf(_last_error, sizeof(_last_error), "HID read: timed out");
        return false;
    }
    memcpy(reply, _report, HID_REPORT_SIZE);
    return true;
}
