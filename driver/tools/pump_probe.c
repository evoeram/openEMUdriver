// pump_probe — reproduces the pump stall without the audio engine: starts the
// M3 pattern test stream on the control IOCTL, polls the stats for a few
// seconds, stops. A healthy pump keeps submitting forever (capture is fully
// self-sustaining; render repeats the pattern). A frozen pump shows Submits
// freezing while Running stays 1 — URBs parked in the controller.
//
// Run elevated (the control device is admin-only by SDDL).
//
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <stdio.h>
#include "emu_ioctl.h"

static HANDLE OpenCtl(void)
{
    HANDLE h = CreateFileA("\\\\.\\EmuUSBAudioCtl", GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("open \\\\.\\EmuUSBAudioCtl failed, err %lu\n", GetLastError());
    }
    return h;
}

static BOOL ReadStats(HANDLE h, EMU_TEST_STREAM_STATS* s)
{
    DWORD ret;
    BOOL ok = DeviceIoControl(h, EMU_IOCTL_TEST_STREAM_STATS, NULL, 0,
                              s, sizeof(*s), &ret, NULL);
    if (!ok || ret < sizeof(*s)) {
        printf("stats failed, err %lu ret %lu\n", GetLastError(), ret);
    }
    return ok;
}

static void Show(HANDLE h, const char* tag)
{
    EMU_TEST_STREAM_STATS s;
    if (!ReadStats(h, &s)) {
        return;
    }
    printf("[%s] rate=%lu | render: run=%lu sub=%lu cmp=%lu fail=%lu perr=%lu "
           "short=%lu bytes=%lu lastUrb=0x%lx lastPkt=0x%lx | capture: "
           "run=%lu sub=%lu cmp=%lu fail=%lu\n",
           tag, s.RateHz,
           s.Dir[0].Running, s.Dir[0].Submits, s.Dir[0].Completes,
           s.Dir[0].UrbFailures, s.Dir[0].IsoPacketErrors,
           s.Dir[0].ShortPackets, s.Dir[0].Bytes, s.Dir[0].LastUrbStatus,
           s.Dir[0].LastPacketStatus,
           s.Dir[1].Running, s.Dir[1].Submits, s.Dir[1].Completes,
           s.Dir[1].UrbFailures);
}

int main(void)
{
    EMU_SET_INPUT in;
    EMU_TEST_STREAM_STATS s;
    HANDLE h;
    DWORD ret;
    int step;

    h = OpenCtl();
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    Show(h, "baseline");

    in.Size = sizeof(in);
    in.Version = EMU_IOCTL_VERSION;
    in.Value = 48000;
    if (!DeviceIoControl(h, EMU_IOCTL_TEST_STREAM_START, &in, sizeof(in),
                         &s, sizeof(s), &ret, NULL)) {
        printf("test start failed, err %lu\n", GetLastError());
        return 1;
    }
    printf("test start: status 0x%lx\n", s.Size ? 0 : 0);

    for (step = 1; step <= 8; step++) {
        Sleep(500);
        Show(h, "run");
    }

    if (!DeviceIoControl(h, EMU_IOCTL_TEST_STREAM_STOP, &in, sizeof(in),
                         &s, sizeof(s), &ret, NULL)) {
        printf("test stop failed, err %lu\n", GetLastError());
    } else {
        printf("test stop ok\n");
    }
    Show(h, "after-stop");
    (VOID)CloseHandle(h);
    return 0;
}
