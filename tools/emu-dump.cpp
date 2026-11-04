// emu-dump — diagnostic CLI for openEMUdriver
// Lists E-MU USB devices, dumps UAC1 layout + XU state, tests streaming config.
//   emu-dump              summarize attached devices
//   emu-dump -v           (reserved for more detail)
//   emu-dump -o [dir]     also save a raw descriptor dump per device
//                         (default dir: docs/descriptors)
// Build: see build.sh
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <direct.h>
#include "emu_usb.h"

using namespace oed;

static void hexDump(FILE* f, const uint8_t* p, uint16_t len) {
    for (uint16_t off = 0; off < len; off += 16) {
        fprintf(f, "%04X  ", off);
        uint16_t n = (len - off < 16) ? (uint16_t)(len - off) : 16;
        for (uint16_t i = 0; i < 16; i++)
            fprintf(f, i < n ? "%02X " : "   ", p[off + i]);
        fprintf(f, " |");
        for (uint16_t i = 0; i < n; i++) {
            uint8_t c = p[off + i];
            fprintf(f, "%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        fprintf(f, "|\n");
    }
}

// Saved dump = raw configuration descriptor + the layout this tool parsed out
// of it. It is the fixture the kernel-side descriptor parser (M2) and its unit
// tests are written against, so the device does not have to be attached.
static bool saveDump(const char* dir, DeviceInfo& di, EmuDevice& dev) {
    char path[512];
    snprintf(path, sizeof(path), "%s/VID_%04X_PID_%04X.txt", dir, di.vid, di.pid);
    FILE* f = fopen(path, "wb");
    if (!f) { printf("    cannot write %s (GLE=%lu)\n", path, GetLastError()); return false; }

    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "# openEMUdriver emu-dump descriptor dump\n");
    fprintf(f, "# date: %04u-%02u-%02u %02u:%02u:%02u\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fprintf(f, "# model: %s  VID_%04X PID_%04X\n", di.name, di.vid, di.pid);
    fprintf(f, "# path: %ls\n", di.path);
    fprintf(f, "# NOTE: interfaces are vendor-class 0xFF but carry the standard\n"
              "# UAC1 class-specific layout (firmware CA0189 hides them from the\n"
              "# in-box usbaudio.sys — see docs/RE_NOTES.md).\n\n");

    fprintf(f, "raw configuration descriptor, wTotalLength=%u\n", dev.rawConfigLength());
    hexDump(f, dev.rawConfig(), dev.rawConfigLength());

    static const char* dirName[2] = { "OUT (render)", "IN (capture)" };
    for (int d = 0; d < 2; d++) {
        fprintf(f, "\n%s formats (%d):\n", dirName[d], dev.formatCount(d));
        const StreamFormat* fs = dev.formats(d);
        for (int j = 0; j < dev.formatCount(d); j++)
            fprintf(f, "  alt=%u rate=%u ch=%u bits=%u maxPacket=%u\n",
                    fs[j].altSetting, fs[j].rate, fs[j].channels, fs[j].bitDepth, fs[j].maxPacket);
    }

    static const char* xuNames[6] = { "ClockRate", "ClockSource", "DigitalIO",
                                      "DeviceOptions", "DirectMonitor", "Metering" };
    fprintf(f, "\nAudioControl interface: 0x%02X\nXU units:", dev.acInterface());
    for (int u = 0; u < 6; u++)
        fprintf(f, "  %s=0x%02X", xuNames[u], dev.xuUnit((XUCode)(0xE301 + u)));
    fprintf(f, "\n");

    uint8_t cs = 0, lock = 0, sl = 0, fmt = 0;
    uint32_t dr = 0;
    fprintf(f, "\nXU state (last confirmed SET — firmware implements no GET_CUR,\n"
              "so these reflect the driver cache, not the device, when unset):\n");
    if (dev.getClockSource(cs)) fprintf(f, "clock source: %s\n", cs ? "S/PDIF external" : "Internal");
    if (dev.getDigRate(dr))     fprintf(f, "digital io rate: %u\n", dr);
    if (dev.getDigLock(lock))   fprintf(f, "digital lock: %s\n", lock ? "locked" : "unlocked");
    if (dev.getSoftLimit(sl))   fprintf(f, "soft limit: %s\n", sl ? "on" : "off");
    if (dev.getSpdifFormat(fmt))
        fprintf(f, "spdif out format: %s\n", fmt == 1 ? "SPDIF" : fmt == 2 ? "AES/EBU" : "None");

    fclose(f);
    printf("    dump saved: %s\n", path);
    return true;
}

int main(int argc, char** argv) {
    const char* dumpDir = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o")) {
            dumpDir = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "docs/descriptors";
        }
    }

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

        if (dumpDir) {
            _mkdir(dumpDir);              // already there is fine
            saveDump(dumpDir, list[i], dev);
        }
        dev.close();
    }
    if (argc > 1 && !strcmp(argv[1], "-v"))
        printf("\nverbose: nothing more yet\n");
    return n > 0 ? 0 : 2;
}
