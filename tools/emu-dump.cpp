// emu-dump — diagnostic CLI for openEMUdriver
// Lists E-MU USB devices, dumps UAC1 layout + XU state, tests streaming config.
// Build: see build.sh
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <stdio.h>
#include "emu_usb.h"

using namespace oed;

int main(int argc, char** argv) {
    DeviceInfo list[8];
    int n = enumerateDevices(list, 8);
    printf("openEMUdriver emu-dump\nE-MU USB devices found: %d\n", n);
    for (int i = 0; i < n; i++) {
        printf("\n[%d] %s  VID_%04X PID_%04X\n", i, list[i].name, list[i].vid, list[i].pid);
        printf("    path: %ls\n", list[i].path);

        EmuDevice dev;
        if (!dev.open(list[i])) {
            printf("    OPEN FAILED (GLE=%lu) — is openEMUdriver INF bound to this device?\n", GetLastError());
            continue;
        }
        printf("    opened OK\n");

        for (int dir = 0; dir < 2; dir++) {
            const StreamFormat* fs = dev.formats(dir);
            int cnt = dev.formatCount(dir);
            printf("    %s formats (%d):\n", dir == 0 ? "OUT" : "IN", cnt);
            for (int j = 0; j < cnt; j++)
                printf("      alt=%u rate=%u ch=%u bits=%u maxPacket=%u\n",
                       fs[j].altSetting, fs[j].rate, fs[j].channels, fs[j].bitDepth, fs[j].maxPacket);
        }

        static const char* xuNames[6] = { "ClockRate", "ClockSource", "DigitalIO",
                                          "DeviceOptions", "DirectMonitor", "Metering" };
        printf("    XU units:");
        for (int u = 0; u < 6; u++) {
            uint8_t id = dev.xuUnit((XUCode)(0xE301 + u));
            printf("  %s=0x%02X", xuNames[u], id);
        }
        printf("\n");

        uint8_t cs = 0;
        if (dev.getClockSource(cs)) printf("    clock source: %s\n", cs ? "S/PDIF external" : "Internal");
        uint32_t dr = 0;
        if (dev.getDigRate(dr)) printf("    digital io rate: %u\n", dr);
        uint8_t lock = 0;
        if (dev.getDigLock(lock)) printf("    digital lock: %s\n", lock ? "locked" : "unlocked");
        uint8_t sl = 0;
        if (dev.getSoftLimit(sl)) printf("    soft limit: %s\n", sl ? "on" : "off");
        uint8_t fmt = 0;
        if (dev.getSpdifFormat(fmt))
            printf("    spdif out format: %s\n", fmt == 1 ? "SPDIF" : fmt == 2 ? "AES/EBU" : "None");
        dev.close();
    }
    if (argc > 1 && !strcmp(argv[1], "-v"))
        printf("\nverbose: nothing more yet\n");
    return n > 0 ? 0 : 2;
}
