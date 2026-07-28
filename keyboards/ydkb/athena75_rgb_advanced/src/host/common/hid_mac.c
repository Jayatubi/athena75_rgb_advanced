// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// macOS raw-HID backend using only the native IOKit HID Manager + CoreFoundation
// (no third-party deps). Matches vid/pid + primary usage page/usage, sends output
// reports with IOHIDDeviceSetReport, and receives input reports via a run-loop
// scheduled callback with a timeout.

#include "hid.h"
#include "proto.h"

#include <stdlib.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>

struct hid_dev {
    IOHIDDeviceRef dev;
    uint8_t        inbuf[64];   // OS input-report scratch
    uint8_t        last[ATHENA_REPORT_LEN];
    volatile int   got;
};

static int cf_num(IOHIDDeviceRef d, CFStringRef key) {
    CFTypeRef v = IOHIDDeviceGetProperty(d, key);
    int out = -1;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &out);
    return out;
}

static void input_cb(void *ctx, IOReturn res, void *sender, IOHIDReportType type,
                     uint32_t reportID, uint8_t *report, CFIndex len) {
    (void)res; (void)sender; (void)type; (void)reportID;
    hid_dev *d = (hid_dev *)ctx;
    CFIndex n = len < ATHENA_REPORT_LEN ? len : ATHENA_REPORT_LEN;
    memset(d->last, 0, sizeof d->last);
    if (n > 0) memcpy(d->last, report, (size_t)n);
    d->got = 1;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

hid_dev *hid_native_open(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!mgr) return NULL;

    int ivid = vid, ipid = pid;
    CFNumberRef nvid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ivid);
    CFNumberRef npid = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ipid);
    CFStringRef keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey)};
    CFTypeRef vals[] = {nvid, npid};
    CFDictionaryRef match = CFDictionaryCreate(kCFAllocatorDefault, (const void **)keys,
                                               (const void **)vals, 2,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(mgr, match);
    CFRelease(match); CFRelease(nvid); CFRelease(npid);

    if (IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        CFRelease(mgr);
        return NULL;
    }

    hid_dev *out = NULL;
    CFSetRef set = IOHIDManagerCopyDevices(mgr);
    if (set) {
        CFIndex cnt = CFSetGetCount(set);
        IOHIDDeviceRef *arr = (IOHIDDeviceRef *)calloc(cnt, sizeof(IOHIDDeviceRef));
        if (arr) {
            CFSetGetValues(set, (const void **)arr);
            for (CFIndex i = 0; i < cnt && !out; i++) {
                IOHIDDeviceRef d = arr[i];
                if (cf_num(d, CFSTR(kIOHIDPrimaryUsagePageKey)) == (int)usage_page &&
                    cf_num(d, CFSTR(kIOHIDPrimaryUsageKey)) == (int)usage &&
                    IOHIDDeviceOpen(d, kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
                    out = (hid_dev *)calloc(1, sizeof(*out));
                    if (out) {
                        out->dev = d;
                        CFRetain(d);
                        IOHIDDeviceRegisterInputReportCallback(d, out->inbuf, sizeof out->inbuf,
                                                               input_cb, out);
                        IOHIDDeviceScheduleWithRunLoop(d, CFRunLoopGetCurrent(),
                                                       kCFRunLoopDefaultMode);
                    }
                }
            }
            free(arr);
        }
        CFRelease(set);
    }
    // Keep the manager alive for the device's lifetime (leak is fine for a CLI).
    return out;
}

int hid_native_write(hid_dev *d, const uint8_t *data) {
    if (!d) return -1;
    IOReturn r = IOHIDDeviceSetReport(d->dev, kIOHIDReportTypeOutput, 0 /*report id*/,
                                      data, ATHENA_REPORT_LEN);
    return (r == kIOReturnSuccess) ? 0 : -1;
}

int hid_native_read(hid_dev *d, uint8_t *data, int timeout_ms) {
    if (!d) return -1;
    d->got = 0;
    CFTimeInterval t = (timeout_ms < 0) ? 3600.0 : (timeout_ms / 1000.0);
    // returnAfterSourceHandled=true so we wake as soon as the callback stops us.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, t, true);
    if (d->got) {
        memcpy(data, d->last, ATHENA_REPORT_LEN);
        return 1;
    }
    return 0;
}

void hid_native_close(hid_dev *d) {
    if (!d) return;
    if (d->dev) {
        IOHIDDeviceUnscheduleFromRunLoop(d->dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        IOHIDDeviceClose(d->dev, kIOHIDOptionsTypeNone);
        CFRelease(d->dev);
    }
    free(d);
}
