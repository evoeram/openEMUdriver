// stats_dump — one-shot reader of the EmuIoctlTestStreamStats reply from the
// driver's control device. Run while a stream is live or right after a stop:
// the counters survive the stop until the next start. The control device is
// a named object with a symlink (DriverEntry: IoCreateDeviceSecure +
// IoCreateSymbolicLink), admin-only by SDDL — run elevated.
//
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <stdio.h>
#include "emu_ioctl.h"

static const char* StatName[] = {
    "Running", "InterfaceNumber", "AltSetting", "MaxPacketSize",
    "PipeInterval", "PeriodMs", "NominalFrames", "Submits", "Completes",
    "UrbFailures", "IsoPacketErrors", "ShortPackets", "Bytes",
    "LastUrbStatus", "LastPacketStatus"
};

int main(void)
{
    HANDLE h;
    EMU_TEST_STREAM_STATS stats;
    DWORD ret, d, f;
    BOOL ok;

    h = CreateFileA("\\\\.\\EmuUSBAudioCtl", GENERIC_READ | GENERIC_WRITE, 0,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("open \\\\.\\EmuUSBAudioCtl failed, err %lu\n", GetLastError());
        return 1;
    }

    ok = DeviceIoControl(h, EMU_IOCTL_TEST_STREAM_STATS, NULL, 0,
                         &stats, sizeof(stats), &ret, NULL);
    if (!ok || ret < sizeof(stats)) {
        printf("stats failed, err %lu ret %lu\n", GetLastError(), ret);
        return 1;
    }
    printf("Size=%lu Version=%lu RateHz=%lu\n",
           stats.Size, stats.Version, stats.RateHz);
    for (d = 0; d < 2; d++) {
        const ULONG* v = (const ULONG*)&stats.Dir[d];
        printf("--- dir %lu (%s)\n", d, d == 0 ? "render" : "capture");
        for (f = 0; f < sizeof(EMU_TEST_STREAM_DIR_STATS) / sizeof(ULONG); f++) {
            printf("  %-16s = 0x%08lx (%lu)\n", StatName[f], v[f], v[f]);
        }
    }
    (VOID)CloseHandle(h);
    return 0;
}
