// emu-probe — user-mode client for the EmuUSBAudio control device (M2 4c).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The panel does not exist yet; this tool is its stand-in for the hardware run.
// It opens the driver-owned control device — the hardware run of 2026-11-04
// proved that IRP_MJ_CREATE on the adapter FDO is answered by KS and rejected,
// so the panel goes through \\.\EmuUSBAudioCtl instead — and issues both
// queries of driver/emuusbaudio/Emu/emu_ioctl.h, the same header a real panel
// would compile, printing every field with its "unknown" semantics intact.
// Exit code 0 when both IOCTLs were answered.
//
// Everything here is read-only by construction: the contract has no write codes
// yet, so the worst this tool can do is ask questions the driver refuses.
#define WIN32_NO_STATUS        // STATUS_* must come from ntstatus.h, not winnt.h
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu_ioctl.h"

static const char* PnpStateName(ULONG s)
{
    switch (s) {
    case EMU_WIRE_STATE_NOT_STARTED:      return "not-started";
    case EMU_WIRE_STATE_STARTED:          return "started";
    case EMU_WIRE_STATE_STOPPED:          return "stopped";
    case EMU_WIRE_STATE_SURPRISE_REMOVED: return "surprise-removed";
    case EMU_WIRE_STATE_REMOVED:          return "removed";
    default:                              return "?";
    }
}

static void PrintMask(const char* what, ULONG mask)
{
    printf("  %-16s 0x%08X :", what, mask);
    if (mask & EMU_OP_GET_CAPS)         printf(" get-caps");
    if (mask & EMU_OP_GET_STATE)        printf(" get-state");
    if (mask & EMU_OP_SET_RATE)         printf(" set-rate");
    if (mask & EMU_OP_SET_CLOCK_SOURCE) printf(" set-clock");
    if (mask & EMU_OP_SET_SPDIF_FORMAT) printf(" set-spdif");
    if (mask & EMU_OP_SET_SOFT_LIMIT)   printf(" set-softlimit");
    if (mask & EMU_OP_GET_STREAM_STATS) printf(" stream-stats");
    printf("\n");
}

static void PrintCaps(const EMU_CAPS_INFO* c)
{
    ULONG i;

    printf("EMU_CAPS_INFO: size %lu, version %lu\n", c->Size, c->Version);
    PrintMask("implemented", c->ImplementedMask);
    PrintMask("supported", c->SupportedMask);
    printf("  parsed-flags    0x%08X\n", c->ParsedFlags);
    printf("  config          %lu, AC interface %lu\n",
           c->ConfigurationValue, c->AcInterfaceNumber);
    printf("  alts            %lu render, %lu capture\n",
           c->RenderAltCount, c->CaptureAltCount);
    printf("  channels max    %lu\n", c->MaxChannels);
    printf("  rates           %lu-%lu Hz", c->MinRateHz, c->MaxRateHz);
    if (c->RateFlags & EMU_RATES_CONTINUOUS) {
        printf("  [continuous, list empty by definition]\n");
    } else {
        printf("  [%lu discrete]", c->RateCount);
        if (c->RateFlags & EMU_RATES_TRUNCATED) {
            printf(" [truncated]");
        }
        printf("\n ");
        for (i = 0; i < c->RateCount; i++) {
            printf(" %lu", c->Rates[i]);
        }
        printf("\n");
    }
    printf("  units           rate 0x%02X, clock 0x%02X, spdif 0x%02X, "
           "softlimit 0x%02X, monitor 0x%02X, metering 0x%02X\n",
           c->ClockRateUnitId, c->ClockSourceUnitId, c->DigitalIoUnitId,
           c->DeviceOptionsUnitId, c->DirectMonitorUnitId, c->MeteringUnitId);
    printf("  feature unit    count %lu", c->FeatureUnitCount);
    if (c->FeatureUnitId != EMU_NO_VALUE) {
        printf(", id 0x%02X, master controls 0x%02X",
               c->FeatureUnitId, c->FeatureMasterControls);
    }
    printf("\n  status endpoint ");
    if (c->StatusEndpoint == EMU_NO_VALUE) {
        printf("none\n");
    } else {
        printf("0x%02X\n", c->StatusEndpoint);
    }
}

static void PrintState(const EMU_STATE_INFO* s)
{
    printf("EMU_STATE_INFO: size %lu, version %lu\n", s->Size, s->Version);
    printf("  valid-mask      0x%08X:", s->ValidMask);
    if (s->ValidMask & EMU_STATE_VALID_RATE)         printf(" rate");
    if (s->ValidMask & EMU_STATE_VALID_CLOCK_SOURCE) printf(" clock");
    if (s->ValidMask & EMU_STATE_VALID_SPDIF_FORMAT) printf(" spdif");
    if (s->ValidMask & EMU_STATE_VALID_SOFT_LIMIT)   printf(" softlimit");
    printf("\n");
    printf("  rate            ");
    if (s->RateHz == EMU_NO_VALUE) printf("unknown\n"); else printf("%lu Hz\n", s->RateHz);
    printf("  clock source    ");
    if (s->ClockSource == EMU_NO_VALUE) printf("unknown\n"); else printf("%lu\n", s->ClockSource);
    printf("  spdif format    ");
    if (s->SpdifFormat == EMU_NO_VALUE) printf("unknown\n"); else printf("%lu\n", s->SpdifFormat);
    printf("  soft limit      ");
    if (s->SoftLimit == EMU_NO_VALUE) printf("unknown\n"); else printf("%lu\n", s->SoftLimit);
    printf("  state-sequence  %lu\n", s->StateSequence);
    printf("  last op status  0x%08X\n", s->LastOperationStatus);
    printf("  pnp state       %s (%lu)\n", PnpStateName(s->PnpState), s->PnpState);
    printf("  counters        start %lu, stop %lu, remove %lu\n",
           s->StartCount, s->StopCount, s->RemoveCount);
}

static void PrintWin32Error(const char* what, DWORD err)
{
    LPSTR msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPSTR)&msg, 0, NULL);
    printf("%s failed, Win32 error %lu (%s)\n", what,
           (unsigned long)err, msg ? msg : "no text");
    if (msg) {
        LocalFree(msg);
    }
    if (err == ERROR_ACCESS_DENIED) {
        printf("  (the device object is admin-only: run this tool elevated)\n");
    } else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_SUCH_DEVICE) {
        printf("  (interface registered but disabled, or the instance is not "
               "started - check the driver state)\n");
    }
}

// One SET roundtrip: input stage in, refreshed state out. The verdict is the
// state reply itself - LastOperationStatus for the device answer and the
// field for the requested control (FieldOffset into EMU_STATE_INFO), which
// shows whether the cache took the value (a refused value stays unknown or
// keeps its old one).
static int DoSet(HANDLE h, ULONG code, ULONG value, const char* name,
                 ULONG fieldOffset)
{
    EMU_SET_INPUT in;
    EMU_STATE_INFO state;
    DWORD got;
    ULONG seen;

    in.Size = sizeof(in);
    in.Version = EMU_IOCTL_VERSION;
    in.Value = value;
    if (!DeviceIoControl(h, code, &in, sizeof(in),
                         &state, sizeof(state), &got, NULL)) {
        PrintWin32Error(name, GetLastError());
        return 1;
    }
    if (got != sizeof(state)) {
        printf("%s returned %lu bytes, expected %lu\n", name,
               (unsigned long)got, (unsigned long)sizeof(state));
        return 1;
    }
    printf("%s %lu -> last op status 0x%08X\n", name, value,
           state.LastOperationStatus);
    PrintState(&state);
    if (state.LastOperationStatus != 0) {
        printf("%s: device did not accept the value\n", name);
        return 1;
    }
    seen = *(ULONG*)((PUCHAR)&state + fieldOffset);
    if (seen != value) {
        printf("%s: cache holds %lu, want %lu\n", name, seen, value);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    HANDLE h;
    EMU_CAPS_INFO caps;
    EMU_STATE_INFO state;
    DWORD got;
    int failures = 0;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    // The control device: driver-global, admin-only, present from DriverEntry
    // to unload. A CREATE here is answered by the driver itself (stage 4c) —
    // the hardware run showed the FDO's KS dispatch rejecting CREATE, which is
    // exactly why this device exists.
    h = CreateFileA("\\\\.\\EmuUSBAudioCtl",
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        PrintWin32Error("CreateFile(control device)", err);
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            printf("  (the driver is not loaded: is openemu-ks.inf installed?)\n");
        }
        return 1;
    }

    printf("control device opened\n");

    if (argc >= 2) {
        ULONG value = (argc >= 3) ? strtoul(argv[2], NULL, 0) : 0;
        int r;
        if (!strcmp(argv[1], "set-rate")) {
            r = DoSet(h, EMU_IOCTL_SET_RATE, value, "SET_RATE",
                      offsetof(EMU_STATE_INFO, RateHz));
        } else if (!strcmp(argv[1], "set-clock")) {
            r = DoSet(h, EMU_IOCTL_SET_CLOCK_SOURCE, value, "SET_CLOCK_SOURCE",
                      offsetof(EMU_STATE_INFO, ClockSource));
        } else if (!strcmp(argv[1], "set-spdif")) {
            r = DoSet(h, EMU_IOCTL_SET_SPDIF_FORMAT, value, "SET_SPDIF_FORMAT",
                      offsetof(EMU_STATE_INFO, SpdifFormat));
        } else if (!strcmp(argv[1], "set-softlimit")) {
            r = DoSet(h, EMU_IOCTL_SET_SOFT_LIMIT, value, "SET_SOFT_LIMIT",
                      offsetof(EMU_STATE_INFO, SoftLimit));
        } else {
            printf("unknown subcommand %s; reading state only\n", argv[1]);
            r = -1;
        }
        if (r >= 0) {
            CloseHandle(h);
            return r;
        }
    }

    memset(&caps, 0, sizeof(caps));
    if (!DeviceIoControl(h, EMU_IOCTL_GET_CAPS, NULL, 0,
                         &caps, sizeof(caps), &got, NULL)) {
        PrintWin32Error("DeviceIoControl(GET_CAPS)", GetLastError());
        failures++;
    } else {
        if (got != sizeof(caps)) {
            printf("GET_CAPS returned %lu bytes, expected %lu\n",
                   (unsigned long)got, (unsigned long)sizeof(caps));
            failures++;
        }
        PrintCaps(&caps);
    }

    printf("\n");

    memset(&state, 0, sizeof(state));
    if (!DeviceIoControl(h, EMU_IOCTL_GET_STATE, NULL, 0,
                         &state, sizeof(state), &got, NULL)) {
        PrintWin32Error("DeviceIoControl(GET_STATE)", GetLastError());
        failures++;
    } else {
        if (got != sizeof(state)) {
            printf("GET_STATE returned %lu bytes, expected %lu\n",
                   (unsigned long)got, (unsigned long)sizeof(state));
            failures++;
        }
        PrintState(&state);
    }

    CloseHandle(h);
    printf("\nemu-probe: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
