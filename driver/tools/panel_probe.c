// panel-probe — user-mode replica of the E-MU panel's private IOCTL path
// (docs/PANEL_IOCTL.md). Talks KSPROPERTY {F6D7C04D} to emusba10 and runs the
// panel's device sequence: descriptor, vendor EP0 status/state, sync params,
// START. SPDX-License-Identifier: MIT.
#define WIN32_NO_STATUS
#include <windows.h>
#include <stdio.h>
#include <initguid.h>
#include <setupapi.h>

DEFINE_GUID(GUID_KSCAT_RENDER, 0x65e8773e, 0x8f56, 0x11d0, 0xa3, 0xb9, 0x00,
            0xa0, 0xc9, 0x22, 0x31, 0x96);
DEFINE_GUID(GUID_EMU_CONTROL, 0x4b8f9afb, 0xee9f, 0x4890, 0x94, 0xec, 0x63,
            0x64, 0x14, 0xbd, 0x37, 0x68);
DEFINE_GUID(GUID_EMU_FILTER2, 0x50894fa4, 0x9206, 0x4a91, 0x87, 0x28, 0xcf,
            0x6f, 0x26, 0x8f, 0x27, 0x1d);
// {F6D7C04D-C5C3-4181-9ED7-504ABE0CDDFF}
DEFINE_GUID(GUID_F6D7C04D, 0xf6d7c04d, 0xc5c3, 0x4181, 0x9e, 0xd7, 0x50,
            0x4a, 0xbe, 0x0c, 0xdd, 0xff);

#define IOCTL_KS_PROPERTY 0x2F0003

#pragma pack(push, 1)
typedef struct {
    GUID  Set;
    ULONG Id;
    ULONG Flags;      // 1 = GET, 2 = SET
    // payload at +0x18
    UCHAR UrbFunction;   // Id 7: panel sends 0x20
    UCHAR BRequest;
    USHORT WValue;
    USHORT WIndex;
    ULONG  _pad;
    ULONG  DataSize;     // SET Id 7
    UCHAR  Data[0x100];
} PANEL_REQ;
#pragma pack(pop)

static HANDLE g_h;

static BOOL
KsProp(ULONG id, ULONG flags, void* payload, ULONG payloadLen,
       void* out, ULONG outLen, ULONG* got)
{
    PANEL_REQ req;
    ULONG bytes = 0;

    ZeroMemory(&req, sizeof(req));
    req.Set = GUID_F6D7C04D;
    req.Id = id;
    req.Flags = flags;
    if (payloadLen > 0x108) {
        return FALSE;
    }
    CopyMemory(&req.UrbFunction, payload, payloadLen);
    return DeviceIoControl(g_h, IOCTL_KS_PROPERTY, &req,
                           (DWORD)(0x18 + payloadLen), out, outLen,
                           got ? got : &bytes, NULL);
}

static void
Dump(const char* what, const UCHAR* p, ULONG n)
{
    printf("%s (%lu):", what, (unsigned long)n);
    for (ULONG i = 0; i < n && i < 64; i++) {
        printf(" %02x", p[i]);
    }
    printf("\n");
}

int
main(int argc, char** argv)
{
    UCHAR out[0x100];
    ULONG got = 0;
    UCHAR payload[0x18];
    BOOL ok;
    HDEVINFO info;
    SP_DEVICE_INTERFACE_DATA ifd;
    SP_DEVICE_INTERFACE_DETAIL_DATA_W* detail;
    DWORD idx, size;
    BOOL found = FALSE;

    // Enumerate the KS render-filter interfaces of the E-MU (MI_00 child):
    // the START property (Id 0x20) lives on the audio filter (tables A),
    // not on the control filter.
    info = SetupDiGetClassDevsW(&GUID_KSCAT_RENDER, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        printf("SetupDiGetClassDevs failed, err %lu\n", GetLastError());
        return 1;
    }
    detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)
        LocalAlloc(LMEM_ZEROINIT, 1024);
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    for (idx = 0; !found; idx++) {
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(info, NULL, &GUID_KSCAT_RENDER, idx,
                                         &ifd)) {
            break;
        }
        SetupDiGetDeviceInterfaceDetailW(info, &ifd, detail, 1024, NULL,
                                         NULL);
        if (wcsstr(detail->DevicePath, L"mi_00") == NULL) {
            continue;
        }
        printf("interface[%lu]: %ls\n", (unsigned long)idx,
               detail->DevicePath);
        g_h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                          0, NULL, OPEN_EXISTING, 0, NULL);
        if (g_h != INVALID_HANDLE_VALUE) {
            found = TRUE;
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    LocalFree(detail);
    if (!found) {
        printf("no openable control filter interface\n");
        return 1;
    }
    printf("control filter opened\n");

    // 1. GET Id 0: device descriptor (18 bytes) — sanity check.
    ZeroMemory(out, sizeof(out));
    ok = KsProp(0x00, 1, NULL, 0, out, 18, &got);
    printf("GET Id0 (desc): ok=%d got=%lu\n", ok, got);
    if (ok) {
        Dump("desc", out, got);
    }

    // 2. GET Id 7: vendor status — bRequest 0x2A, 1 byte.
    ZeroMemory(payload, sizeof(payload));
    payload[0] = 0x20;    // UrbFunction (panel value)
    payload[1] = 0x2A;    // bRequest
    // wValue=0, wIndex=0
    ZeroMemory(out, sizeof(out));
    got = 0;
    ok = KsProp(0x07, 1, payload, 0x18, out, 1, &got);
    printf("GET Id7 0x2A (status): ok=%d got=%lu err=%lu\n", ok, got,
           ok ? 0 : GetLastError());
    if (ok && got) {
        Dump("status", out, got);
    }

    // 3. GET Id 7: vendor state block — bRequest 0x01, 32 bytes.
    ZeroMemory(payload, sizeof(payload));
    payload[0] = 0x20;
    payload[1] = 0x01;
    ZeroMemory(out, sizeof(out));
    got = 0;
    ok = KsProp(0x07, 1, payload, 0x18, out, 32, &got);
    printf("GET Id7 0x01 (state32): ok=%d got=%lu err=%lu\n", ok, got,
           ok ? 0 : GetLastError());
    if (ok && got) {
        Dump("state32", out, got);
    }

    // 4. GET Id 0x10002: sync anchor (frameNumber + 3).
    ZeroMemory(out, sizeof(out));
    got = 0;
    ok = KsProp(0x10002, 1, NULL, 0, out, 4, &got);
    printf("GET 0x10002 (anchor): ok=%d got=%lu err=%lu\n", ok, got,
           ok ? 0 : GetLastError());
    if (ok && got >= 4) {
        ULONG v;
        CopyMemory(&v, out, 4);
        printf("  anchor frame: %lu\n", (unsigned long)v);
    }

    // 5. SET Id 0x20: START (the vendor's stream start).
    ZeroMemory(payload, sizeof(payload));
    ok = KsProp(0x20, 2, payload, 0x18, NULL, 0, &got);
    printf("SET Id0x20 (START): ok=%d err=%lu\n", ok, ok ? 0 : GetLastError());

    // 6. GET Id 7 after START: re-read the vendor status/state.
    ZeroMemory(payload, sizeof(payload));
    payload[0] = 0x20;
    payload[1] = 0x2A;
    ZeroMemory(out, sizeof(out));
    got = 0;
    ok = KsProp(0x07, 1, payload, 0x18, out, 1, &got);
    printf("GET Id7 0x2A after START: ok=%d got=%lu\n", ok, got);
    if (ok && got) {
        Dump("status", out, got);
    }

    CloseHandle(g_h);
    printf("done (device is now in the vendor START state)\n");
    return 0;
}
