// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Windows raw-HID backend using only OS-native APIs (SetupAPI + hid.dll).
// Enumerates HID interfaces, matches vid/pid + usage page/usage, and does
// overlapped I/O so reads can time out.

#include "hid.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

struct hid_dev {
    HANDLE h;
    USHORT in_len;   // InputReportByteLength  (incl. leading report id)
    USHORT out_len;  // OutputReportByteLength (incl. leading report id)
};

// Wrap an already-open handle whose caps have been checked.
static hid_dev *wrap(HANDLE h, const HIDP_CAPS *caps) {
    hid_dev *dev = (hid_dev *)calloc(1, sizeof(*dev));
    if (!dev) return NULL;
    dev->h       = h;
    dev->in_len  = caps->InputReportByteLength;
    dev->out_len = caps->OutputReportByteLength;
    // Deep input queue so a streamed burst (screenshot) doesn't overflow the
    // driver's default 32-report ring and drop chunks.
    HidD_SetNumInputBuffers(h, 512);
    return dev;
}

// True when this interface is the vid/pid + usage one we are after; `caps` and
// the product string come back filled so callers need not reopen it.
static int match(HANDLE h, uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage,
                 HIDP_CAPS *caps, char *label, size_t label_len) {
    HIDD_ATTRIBUTES attr;
    attr.Size = sizeof(attr);
    if (!HidD_GetAttributes(h, &attr) || attr.VendorID != vid || attr.ProductID != pid) return 0;

    PHIDP_PREPARSED_DATA ppd = NULL;
    if (!HidD_GetPreparsedData(h, &ppd)) return 0;
    int ok = HidP_GetCaps(ppd, caps) == HIDP_STATUS_SUCCESS && caps->UsagePage == usage_page &&
             caps->Usage == usage;
    HidD_FreePreparsedData(ppd);
    if (!ok) return 0;

    if (label && label_len) {
        WCHAR wide[128] = {0};
        label[0] = '\0';
        if (HidD_GetProductString(h, wide, sizeof wide)) {
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, label, (int)label_len, NULL, NULL);
        }
        if (!label[0]) snprintf(label, label_len, "athena75");
    }
    return 1;
}

int hid_native_list(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage,
                    hid_target *out, int max) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO info = SetupDiGetClassDevsA(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return 0;

    int n = 0;
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; n < max && SetupDiEnumDeviceInterfaces(info, NULL, &guid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(need);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (SetupDiGetDeviceInterfaceDetailA(info, &ifd, det, need, NULL, NULL)) {
            // Shared read/write and no data transfer, so probing never disturbs
            // whatever else has the device open.
            HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                HIDP_CAPS caps;
                char      label[64];
                if (match(h, vid, pid, usage_page, usage, &caps, label, sizeof label)) {
                    hid_target *t = &out[n++];
                    memset(t, 0, sizeof *t);
                    snprintf(t->kind, sizeof t->kind, "usb");
                    snprintf(t->id, sizeof t->id, "usb%d", n);
                    snprintf(t->label, sizeof t->label, "%s", label);
                    snprintf(t->detail, sizeof t->detail, "%s", det->DevicePath);
                }
                CloseHandle(h);
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(info);
    return n;
}

hid_dev *hid_native_open_target(const hid_target *t) {
    if (!t || !t->detail[0]) return NULL;
    HANDLE h = CreateFileA(t->detail, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    PHIDP_PREPARSED_DATA ppd = NULL;
    HIDP_CAPS            caps;
    hid_dev             *dev = NULL;
    if (HidD_GetPreparsedData(h, &ppd) && HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
        dev = wrap(h, &caps);
    }
    if (ppd) HidD_FreePreparsedData(ppd);
    if (!dev) CloseHandle(h);
    return dev;
}

hid_dev *hid_native_open(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage) {
    hid_target t[HID_TARGET_MAX];
    int        n = hid_native_list(vid, pid, usage_page, usage, t, HID_TARGET_MAX);
    return n ? hid_native_open_target(&t[0]) : NULL;
}

int hid_native_write(hid_dev *d, const uint8_t *data) {
    if (!d) return -1;
    USHORT n = d->out_len ? d->out_len : (ATHENA_REPORT_LEN + 1);
    uint8_t buf[256] = {0};
    if (n > sizeof buf) n = sizeof buf;
    buf[0] = 0; // report id
    USHORT copy = (USHORT)(n - 1);
    if (copy > ATHENA_REPORT_LEN) copy = ATHENA_REPORT_LEN;
    memcpy(buf + 1, data, copy);

    OVERLAPPED ov = {0};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;
    DWORD wrote = 0;
    int rc = -1;
    if (WriteFile(d->h, buf, n, &wrote, &ov)) {
        rc = 0;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0 &&
            GetOverlappedResult(d->h, &ov, &wrote, FALSE)) {
            rc = 0;
        } else {
            CancelIo(d->h);
        }
    }
    CloseHandle(ov.hEvent);
    return rc;
}

int hid_native_read(hid_dev *d, uint8_t *data, int timeout_ms) {
    if (!d) return -1;
    USHORT n = d->in_len ? d->in_len : (ATHENA_REPORT_LEN + 1);
    uint8_t buf[256] = {0};
    if (n > sizeof buf) n = sizeof buf;

    OVERLAPPED ov = {0};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return -1;

    int rc;
    DWORD got = 0;
    if (ReadFile(d->h, buf, n, &got, &ov)) {
        rc = 1;
    } else if (GetLastError() == ERROR_IO_PENDING) {
        DWORD w = WaitForSingleObject(ov.hEvent, (DWORD)(timeout_ms < 0 ? INFINITE : timeout_ms));
        if (w == WAIT_OBJECT_0 && GetOverlappedResult(d->h, &ov, &got, FALSE)) {
            rc = 1;
        } else {
            CancelIo(d->h);
            rc = 0; // timeout (or cancelled)
        }
    } else {
        rc = -1;
    }
    CloseHandle(ov.hEvent);
    if (rc == 1) {
        // buf[0] is the report id; payload follows.
        memcpy(data, buf + 1, ATHENA_REPORT_LEN);
    }
    return rc;
}

void hid_native_close(hid_dev *d) {
    if (!d) return;
    if (d->h && d->h != INVALID_HANDLE_VALUE) CloseHandle(d->h);
    free(d);
}
