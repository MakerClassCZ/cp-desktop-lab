// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

// Finding a USB serial port by its device's IDs through IOKit; the rest of the serial port is
// platform/posix/serial_host.c.

#include "serial_host.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#include <IOKit/usb/USBSpec.h>
#include <stdio.h>

// A number property of the service or the first of its parents that has it, or -1.
static int _parent_number(io_object_t service, CFStringRef key) {
    CFTypeRef value = IORegistryEntrySearchCFProperty(service, kIOServicePlane, key, NULL,
        kIORegistryIterateRecursively | kIORegistryIterateParents);
    int n = -1;
    if (value != NULL) {
        if (CFGetTypeID(value) != CFNumberGetTypeID() || !CFNumberGetValue(value, kCFNumberIntType, &n)) {
            n = -1;
        }
        CFRelease(value);
    }
    return n;
}

// The port's /dev/cu.* name, the one that opens without waiting for a carrier.
bool serial_host_find_usb(uint16_t vid, uint16_t pid, char *path, size_t len) {
    io_iterator_t ports;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(kIOSerialBSDServiceValue), &ports) != KERN_SUCCESS) {
        return false;
    }
    bool found = false;
    io_object_t port;
    while (!found && (port = IOIteratorNext(ports)) != 0) {
        if (_parent_number(port, CFSTR(kUSBVendorID)) == vid && _parent_number(port, CFSTR(kUSBProductID)) == pid) {
            CFTypeRef name = IORegistryEntryCreateCFProperty(port, CFSTR(kIOCalloutDeviceKey), NULL, 0);
            if (name != NULL) {
                found = CFStringGetCString(name, path, (CFIndex)len, kCFStringEncodingUTF8);
                CFRelease(name);
            }
        }
        IOObjectRelease(port);
    }
    IOObjectRelease(ports);
    return found;
}
