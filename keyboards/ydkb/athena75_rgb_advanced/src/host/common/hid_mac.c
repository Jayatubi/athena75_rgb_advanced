// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// macOS raw-HID backend using only the native IOKit HID Manager + CoreFoundation
// (no third-party deps). Matches vid/pid + primary usage page/usage, sends output
// reports with IOHIDDeviceSetReport, and receives input reports via a run-loop
// scheduled callback with a timeout.

#include "hid.h"
#include "proto.h"

#include <stdio.h>
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

static void cf_str(IOHIDDeviceRef d, CFStringRef key, char *buf, size_t len) {
    buf[0] = '\0';
    CFTypeRef v = IOHIDDeviceGetProperty(d, key);
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)v, buf, (CFIndex)len, kCFStringEncodingUTF8);
    }
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

// Every interface identity is its IOKit location id, which stays put while the
// board is plugged into the same port.
static void target_of(IOHIDDeviceRef d, int index, hid_target *t) {
    char label[64], serial[64];
    cf_str(d, CFSTR(kIOHIDProductKey), label, sizeof label);
    cf_str(d, CFSTR(kIOHIDSerialNumberKey), serial, sizeof serial);
    memset(t, 0, sizeof *t);
    snprintf(t->kind, sizeof t->kind, "usb");
    snprintf(t->id, sizeof t->id, "usb%d", index);
    snprintf(t->label, sizeof t->label, "%s", label[0] ? label : "athena75");
    snprintf(t->detail, sizeof t->detail, "loc=0x%08x%s%s",
             (unsigned)cf_num(d, CFSTR(kIOHIDLocationIDKey)), serial[0] ? " sn=" : "", serial);
}

// One place that walks the matching devices: with `want` NULL it fills `out`
// and returns NULL, otherwise it opens the device whose detail is `want`.
static hid_dev *walk(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage,
                     hid_target *out, int max, int *count, const char *want) {
    if (count) *count = 0;
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

    hid_dev *dev = NULL;
    CFSetRef set = IOHIDManagerCopyDevices(mgr);
    if (set) {
        CFIndex cnt = CFSetGetCount(set);
        IOHIDDeviceRef *arr = (IOHIDDeviceRef *)calloc(cnt, sizeof(IOHIDDeviceRef));
        if (arr) {
            CFSetGetValues(set, (const void **)arr);
            int n = 0;
            for (CFIndex i = 0; i < cnt && !dev; i++) {
                IOHIDDeviceRef d = arr[i];
                if (cf_num(d, CFSTR(kIOHIDPrimaryUsagePageKey)) != (int)usage_page ||
                    cf_num(d, CFSTR(kIOHIDPrimaryUsageKey)) != (int)usage) {
                    continue;
                }
                hid_target t;
                target_of(d, ++n, &t);
                if (!want) {
                    if (out && n <= max) out[n - 1] = t;
                    if (count) *count = n < max ? n : max;
                    continue;
                }
                if (strcmp(t.detail, want) != 0) continue;
                if (IOHIDDeviceOpen(d, kIOHIDOptionsTypeNone) != kIOReturnSuccess) continue;
                dev = (hid_dev *)calloc(1, sizeof(*dev));
                if (dev) {
                    dev->dev = d;
                    CFRetain(d);
                    IOHIDDeviceRegisterInputReportCallback(d, dev->inbuf, sizeof dev->inbuf,
                                                           input_cb, dev);
                    IOHIDDeviceScheduleWithRunLoop(d, CFRunLoopGetCurrent(),
                                                   kCFRunLoopDefaultMode);
                }
            }
            free(arr);
        }
        CFRelease(set);
    }
    // Keep the manager alive for the device's lifetime (leak is fine for a CLI).
    return dev;
}

int hid_native_list(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage,
                    hid_target *out, int max) {
    int n = 0;
    walk(vid, pid, usage_page, usage, out, max, &n, NULL);
    return n;
}

hid_dev *hid_native_open_target(const hid_target *t) {
    if (!t || !t->detail[0]) return NULL;
    // vid/pid/usage are the only ones this tool ever talks to, so re-derive them
    // from proto.h rather than carrying them in hid_target.
    return walk(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE, NULL, 0, NULL, t->detail);
}

hid_dev *hid_native_open(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage) {
    hid_target t[HID_TARGET_MAX];
    int        n = hid_native_list(vid, pid, usage_page, usage, t, HID_TARGET_MAX);
    return n ? hid_native_open_target(&t[0]) : NULL;
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
