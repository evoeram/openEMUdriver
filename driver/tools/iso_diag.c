// iso_diag — one-shot reader of the EmuIsochDiag reply (P1 capture/run
// diagnostics) from the driver's control device. Prints the recent per-URB
// wire history of both directions: what the builder planned per packet, what
// actually arrived, and the IRP/URB/packet statuses in between. No audio
// content travels in it.
//
// Run while a stream is live or right after a stop: the ring survives the
// stop until the next start. Take several snapshots a second apart and stitch
// them by Seq — each one covers the last 32 completions. The control device
// is admin-only by SDDL — run elevated.
//
// Build (user-mode flags mirror kprobe.sh; the tool compiles the same
// Emu/emu_ioctl.h and Emu/emu_isoch.h the driver ships, so a layout drift is
// a compile error, not a wrong number on screen):
//
//   cl -nologo -W3 -WX -Zi -D_WIN32_WINNT=0x0A00 -DNDEBUG ^
//      /I Emu /I emuusbaudio/Emu ^
//      -Fe emuusbaudio/build/iso-diag.exe -Fo emuusbaudio/build/ ^
//      -Tc tools/iso_diag.c
//
// SPDX-License-Identifier: LGPL-2.1-or-later
#define WIN32_NO_STATUS        // STATUS_* must come from ntstatus.h, not winnt.h
#include <windows.h>
#include <stdio.h>
#include "emu_ioctl.h"

static const char* DirName[2] = { "render", "capture" };

static void DumpStream(ULONG dir, const EMU_ISOCH_DIAG_STREAM* s)
{
    ULONG shown = 0;
    ULONG i;

    printf("--- %s: total %lu recorded since start\n", DirName[dir], s->TotalSeq);
    for (i = 0; i < EMU_ISOCH_DIAG_ROWS; i++) {
        const EMU_ISOCH_DIAG_ROW* r = &s->Row[i];
        ULONG got = r->P[0][2] + r->P[1][2];

        if (r->Seq == 0) {
            continue;
        }
        printf("  seq %-4lu start %-10lu irp %08lx urb %08lx | "
               "p0 plan %lu/%lu got %lu st %08lx | "
               "p1 plan %lu/%lu got %lu st %08lx | sum %lu\n",
               r->Seq, r->StartFrame, r->IrpStatus, r->UrbStatus,
               r->P[0][0], r->P[0][1], r->P[0][2], r->P[0][3],
               r->P[1][0], r->P[1][1], r->P[1][2], r->P[1][3],
               got);
        shown++;
    }
    if (shown == 0) {
        printf("  (no samples - the stream has not completed a data URB\n"
               "   since its last start, or the build predates the diag ring)\n");
    }
}

int main(void)
{
    HANDLE h;
    EMU_ISOCH_DIAG_INFO diag;
    DWORD ret;
    BOOL ok;

    h = CreateFileA("\\\\.\\EmuUSBAudioCtl", GENERIC_READ | GENERIC_WRITE, 0,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("open \\\\.\\EmuUSBAudioCtl failed, err %lu\n", GetLastError());
        return 1;
    }

    ok = DeviceIoControl(h, EMU_IOCTL_TEST_ISOCH_DIAG, NULL, 0,
                         &diag, sizeof(diag), &ret, NULL);
    if (!ok || ret < sizeof(diag)) {
        printf("diag failed, err %lu ret %lu\n", GetLastError(), ret);
        (VOID)CloseHandle(h);
        return 1;
    }
    printf("Size=%lu Version=%lu\n", diag.Size, diag.Version);
    DumpStream(0, &diag.Stream[0]);
    DumpStream(1, &diag.Stream[1]);
    (VOID)CloseHandle(h);
    return 0;
}
