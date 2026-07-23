// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Windows raw-HID backend using only OS-native APIs (SetupAPI + hid.dll).
// Enumerates HID interfaces, matches vid/pid + usage page/usage, and does
// overlapped I/O so reads can time out.

#include "hid.h"
#include "proto.h"

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

hid_dev *hid_open(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO info = SetupDiGetClassDevsA(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return NULL;

    hid_dev *dev = NULL;
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; !dev && SetupDiEnumDeviceInterfaces(info, NULL, &guid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_A *)malloc(need);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (SetupDiGetDeviceInterfaceDetailA(info, &ifd, det, need, NULL, NULL)) {
            HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attr;
                attr.Size = sizeof(attr);
                PHIDP_PREPARSED_DATA ppd = NULL;
                HIDP_CAPS caps;
                if (HidD_GetAttributes(h, &attr) && attr.VendorID == vid && attr.ProductID == pid &&
                    HidD_GetPreparsedData(h, &ppd) && HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == usage_page && caps.Usage == usage) {
                    dev = (hid_dev *)calloc(1, sizeof(*dev));
                    if (dev) {
                        dev->h = h;
                        dev->in_len = caps.InputReportByteLength;
                        dev->out_len = caps.OutputReportByteLength;
                    }
                }
                if (ppd) HidD_FreePreparsedData(ppd);
                if (!dev) CloseHandle(h);
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(info);
    return dev;
}

int hid_write(hid_dev *d, const uint8_t *data) {
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

int hid_read(hid_dev *d, uint8_t *data, int timeout_ms) {
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

void hid_close(hid_dev *d) {
    if (!d) return;
    if (d->h && d->h != INVALID_HANDLE_VALUE) CloseHandle(d->h);
    free(d);
}
