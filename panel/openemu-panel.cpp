// openEMU Panel — lightweight Win32 control panel for E-MU USB (CA0189)
// Talks the UAC1 Extension Unit protocol directly over WinUSB (no driver DLL).
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include "../src/emu_usb.h"

#pragma comment(lib, "comctl32.lib")

using namespace oed;

#define IDC_DEVICE    101
#define IDC_RATE      102
#define IDC_CLOCK     103
#define IDC_SPDIF     104
#define IDC_SOFTLIMIT 105
#define IDC_STATUS    106
#define IDC_REFRESH   107
#define IDC_DIGSTAT   108

static EmuDevice g_dev;
static DeviceInfo g_list[8];
static int g_count = 0;
static int g_cur = -1;
static bool g_busy = false;

static const uint32_t kRates[6] = {44100, 48000, 88200, 96000, 176400, 192000};

static HWND g_hRate, g_hClock, g_hSpdif, g_hSl, g_hStat, g_hDig;

static void setControlsFromDevice(HWND h) {
    char buf[256];
    ComboBox_ResetContent(g_hRate);
    int sel = -1;
    for (int i = 0; i < g_dev.formatCount(0); i++) {
        const StreamFormat* fs = g_dev.formats(0);
        bool dup = false;
        for (int j = 0; j < i; j++) if (fs[j].rate == fs[i].rate) dup = true;
        if (dup) continue;
        sprintf(buf, "%u Hz", fs[i].rate);
        int idx = (int)ComboBox_AddString(g_hRate, buf);
        ComboBox_SetItemData(g_hRate, idx, fs[i].rate);
    }
    // current digital rate as best guess
    uint32_t digRate = 0;
    bool digOk = g_dev.getDigRate(digRate);
    uint8_t cs = 0;
    g_dev.getClockSource(cs);
    if (digOk) {
        for (int i = 0; i < ComboBox_GetCount(g_hRate); i++) {
            if ((uint32_t)ComboBox_GetItemData(g_hRate, i) == digRate) { sel = i; break; }
        }
    }
    if (sel >= 0) ComboBox_SetCurSel(g_hRate, sel);

    ComboBox_ResetContent(g_hClock);
    int i1 = (int)ComboBox_AddString(g_hClock, "Internal");
    ComboBox_SetItemData(g_hClock, i1, 0);
    int i2 = (int)ComboBox_AddString(g_hClock, "S/PDIF external");
    ComboBox_SetItemData(g_hClock, i2, 1);
    ComboBox_SetCurSel(g_hClock, cs ? 1 : 0);

    ComboBox_ResetContent(g_hSpdif);
    int j0 = (int)ComboBox_AddString(g_hSpdif, "None");
    ComboBox_SetItemData(g_hSpdif, j0, 0);
    int j1 = (int)ComboBox_AddString(g_hSpdif, "S/PDIF");
    ComboBox_SetItemData(g_hSpdif, j1, 1);
    int j2 = (int)ComboBox_AddString(g_hSpdif, "AES/EBU");
    ComboBox_SetItemData(g_hSpdif, j2, 2);
    uint8_t f = 0;
    g_dev.getSpdifFormat(f);
    ComboBox_SetCurSel(g_hSpdif, f > 2 ? 0 : f);

    uint8_t sl = 0;
    g_dev.getSoftLimit(sl);
    Button_SetCheck(g_hSl, sl ? BST_CHECKED : BST_UNCHECKED);

    uint8_t lock = 0;
    if (g_dev.getDigLock(lock) && g_dev.getDigRate(digRate))
        sprintf(buf, "Digital input: rate=%u  %s", digRate, lock ? "LOCKED" : "unlocked");
    else
        strcpy(buf, "Digital input: status readout unavailable (firmware implements no GET requests)");
    Static_SetText(g_hDig, buf);
}

static void refreshDevices(HWND h) {
    g_count = enumerateDevices(g_list, 8);
    HWND hDev = GetDlgItem(h, IDC_DEVICE);
    ComboBox_ResetContent(hDev);
    g_cur = -1;
    for (int i = 0; i < g_count; i++) {
        char pathA[512];
        WideCharToMultiByte(CP_ACP, 0, g_list[i].path, -1, pathA, sizeof(pathA), nullptr, nullptr);
        // openable devices only (WinUSB bound)
        EmuDevice t;
        if (!t.open(g_list[i])) continue;
        t.close();
        int idx = (int)ComboBox_AddString(hDev, g_list[i].name);
        ComboBox_SetItemData(hDev, idx, i);
        if (g_cur < 0) { g_cur = i; ComboBox_SetCurSel(hDev, idx); }
    }
    if (g_cur >= 0 && g_dev.open(g_list[g_cur])) {
        setControlsFromDevice(h);
        Static_SetText(g_hStat, "Ready.");
    } else {
        Static_SetText(g_hStat,
            "No openable device. Install inf/openemu.inf (binds WinUSB), then replug the interface.");
    }
}

static void applyDeviceChange(HWND h) {
    if (g_busy) return;
    if (g_cur >= 0) {
        g_dev.close();
        if (g_dev.open(g_list[g_cur])) { setControlsFromDevice(h); Static_SetText(g_hStat, "Ready."); return; }
    }
    Static_SetText(g_hStat, "Device open failed.");
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        CreateWindowA("STATIC", "Device:", WS_CHILD | WS_VISIBLE, 12, 12, 70, 20, h, nullptr, nullptr, nullptr);
        HWND hDev = CreateWindowA("COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  90, 10, 300, 200, h, (HMENU)IDC_DEVICE, nullptr, nullptr);
        CreateWindowA("BUTTON", "Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      400, 10, 80, 24, h, (HMENU)IDC_REFRESH, nullptr, nullptr);
        CreateWindowA("STATIC", "Sample rate:", WS_CHILD | WS_VISIBLE, 12, 48, 90, 20, h, nullptr, nullptr, nullptr);
        g_hRate = CreateWindowA("COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                110, 46, 160, 200, h, (HMENU)IDC_RATE, nullptr, nullptr);
        CreateWindowA("STATIC", "Sync source:", WS_CHILD | WS_VISIBLE, 12, 78, 90, 20, h, nullptr, nullptr, nullptr);
        g_hClock = CreateWindowA("COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                 110, 76, 160, 200, h, (HMENU)IDC_CLOCK, nullptr, nullptr);
        CreateWindowA("STATIC", "S/PDIF out:", WS_CHILD | WS_VISIBLE, 12, 108, 90, 20, h, nullptr, nullptr, nullptr);
        g_hSpdif = CreateWindowA("COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                 110, 106, 160, 200, h, (HMENU)IDC_SPDIF, nullptr, nullptr);
        g_hSl = CreateWindowA("BUTTON", "Soft limit", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              12, 140, 160, 22, h, (HMENU)IDC_SOFTLIMIT, nullptr, nullptr);
        g_hDig = CreateWindowA("STATIC", "Digital input: —", WS_CHILD | WS_VISIBLE,
                               12, 172, 460, 20, h, (HMENU)IDC_DIGSTAT, nullptr, nullptr);
        g_hStat = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE,
                                12, 200, 460, 40, h, (HMENU)IDC_STATUS, nullptr, nullptr);
        CreateWindowA("STATIC",
            "openEMUdriver — direct WinUSB control of E-MU 0202/0404/Tracker Pre USB",
            WS_CHILD | WS_VISIBLE, 12, 240, 460, 20, h, nullptr, nullptr, nullptr);
        (void)hDev;
        refreshDevices(h);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        int code = HIWORD(w);
        if (id == IDC_DEVICE && code == CBN_SELCHANGE) {
            int sel = ComboBox_GetCurSel(GetDlgItem(h, IDC_DEVICE));
            if (sel >= 0) g_cur = (int)ComboBox_GetItemData(GetDlgItem(h, IDC_DEVICE), sel);
            applyDeviceChange(h);
            return 0;
        }
        if (id == IDC_REFRESH && code == BN_CLICKED) { g_dev.close(); refreshDevices(h); return 0; }
        if (!g_dev.isOpen()) return 0;
        if (id == IDC_RATE && code == CBN_SELCHANGE) {
            int sel = ComboBox_GetCurSel(g_hRate);
            if (sel >= 0) {
                uint32_t rate = (uint32_t)ComboBox_GetItemData(g_hRate, sel);
                if (g_dev.setClockRate(rate)) Static_SetText(g_hStat, "Rate set.");
                else Static_SetText(g_hStat, "Rate set failed.");
            }
            return 0;
        }
        if (id == IDC_CLOCK && code == CBN_SELCHANGE) {
            int sel = ComboBox_GetCurSel(g_hClock);
            if (sel >= 0 && g_dev.setClockSource((uint8_t)ComboBox_GetItemData(g_hClock, sel)))
                Static_SetText(g_hStat, "Clock source set.");
            return 0;
        }
        if (id == IDC_SPDIF && code == CBN_SELCHANGE) {
            int sel = ComboBox_GetCurSel(g_hSpdif);
            if (sel >= 0 && g_dev.setSpdifFormat((uint8_t)ComboBox_GetItemData(g_hSpdif, sel)))
                Static_SetText(g_hStat, "S/PDIF format set.");
            return 0;
        }
        if (id == IDC_SOFTLIMIT && code == BN_CLICKED) {
            uint8_t on = Button_GetCheck(g_hSl) == BST_CHECKED ? 1 : 0;
            if (g_dev.setSoftLimit(on)) Static_SetText(g_hStat, on ? "Soft limit ON" : "Soft limit OFF");
            return 0;
        }
        return 0;
    }
    case WM_TIMER: {
        if (g_dev.isOpen()) {
            uint32_t digRate = 0; uint8_t lock = 0;
            char buf[128];
            if (g_dev.getDigRate(digRate) && g_dev.getDigLock(lock))
                sprintf(buf, "Digital input: rate=%u  %s", digRate, lock ? "LOCKED" : "unlocked");
            else
                strcpy(buf, "Digital input: status readout unavailable (no GET in firmware)");
            Static_SetText(g_hDig, buf);
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), 0};
    InitCommonControlsEx(&icc);
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "openEMUPanel";
    RegisterClassA(&wc);
    HWND h = CreateWindowA("openEMUPanel", "openEMU Panel — E-MU USB Audio",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 500, 310,
                           nullptr, nullptr, hi, nullptr);
    ShowWindow(h, SW_SHOW);
    SetTimer(h, 1, 1000, nullptr);
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_dev.close();
    return 0;
}
