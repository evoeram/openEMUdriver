// EmuUSBAudio isoch-scheduling unit test, user-mode.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Build and run:  cd driver && ./utest.sh isoch
//
// The numbers the M3 iso slice lives by are pure functions of the pipe facts
// and the requested rate; this test pins them. Everything here runs against
// the same Emu/emu_isoch.cpp that goes into the image (utest.sh compiles it
// with the kernel flags first), so a scheduler change that breaks the test
// cannot ship.
#include "testharness.h"
#include "emu_isoch.h"

int g_checks = 0;
int g_failures = 0;
const char* g_case = "";

static void CasePeriod(void)
{
    ULONG period = 0;

    g_case = "period";
    // The high-speed reading wins whenever the value is a whole multiple of 8:
    // the 0404's proven 1 ms grid is Interval 8.
    CHECK(EmuIsochPeriodMs(8, &period));
    CHECK_EQ(period, 1);
    CHECK(EmuIsochPeriodMs(16, &period));
    CHECK_EQ(period, 2);
    CHECK(EmuIsochPeriodMs(32, &period));
    CHECK_EQ(period, 4);
    // Full-speed reading: whole frames, including the small values that on a
    // high-speed bus would mean sub-frame periods. The function derives a
    // period, it does not guess the bus speed - the raw interval travels to
    // the TestPipe breadcrumb, and a misread period shows up on the wire as
    // scheduling errors, which is what the breadcrumb is there to explain.
    CHECK(EmuIsochPeriodMs(1, &period));
    CHECK_EQ(period, 1);
    CHECK(EmuIsochPeriodMs(2, &period));
    CHECK_EQ(period, 2);
    CHECK(EmuIsochPeriodMs(4, &period));
    CHECK_EQ(period, 4);
    CHECK(EmuIsochPeriodMs(3, &period));
    CHECK_EQ(period, 3);
    CHECK(EmuIsochPeriodMs(7, &period));
    CHECK_EQ(period, 7);
    // Zero and out-of-range are refusals, and PeriodMs is left untouched.
    period = 99;
    CHECK(!EmuIsochPeriodMs(0, &period));
    CHECK_EQ(period, 99);
    CHECK(!EmuIsochPeriodMs(33, &period));
    CHECK(!EmuIsochPeriodMs(8, NULL));
}

static void CaseNominal(void)
{
    ULONG frames = 0;

    g_case = "nominal";
    // The rates the 0404 lists that divide the 1 ms grid exactly.
    CHECK(EmuIsochNominalFramesExact(48000, 1, &frames));
    CHECK_EQ(frames, 48);
    CHECK(EmuIsochNominalFramesExact(96000, 1, &frames));
    CHECK_EQ(frames, 96);
    CHECK(EmuIsochNominalFramesExact(192000, 1, &frames));
    CHECK_EQ(frames, 192);
    // A two-frame grid doubles the audio each packet must carry: 48000 Hz at
    // 500 packets per second is 96 frames per packet, not 24.
    CHECK(EmuIsochNominalFramesExact(48000, 2, &frames));
    CHECK_EQ(frames, 96);
    // 44.1-family rates need the 44/45 alternation - the M5 feedback engine's
    // business, refused here rather than approximated. On the 2-frame grid
    // 44100 would mean 88.2 frames per packet, equally inexact.
    CHECK(!EmuIsochNominalFramesExact(44100, 1, &frames));
    CHECK(!EmuIsochNominalFramesExact(88200, 1, &frames));
    CHECK(!EmuIsochNominalFramesExact(176400, 1, &frames));
    CHECK(!EmuIsochNominalFramesExact(44100, 2, &frames));
    // Zero rate and a grid that does not divide the second are refusals.
    CHECK(!EmuIsochNominalFramesExact(0, 1, &frames));
    CHECK(!EmuIsochNominalFramesExact(48000, 3, &frames));
    CHECK(!EmuIsochNominalFramesExact(48000, 1, NULL));
}

static void CaseSlotBytes(void)
{
    g_case = "slot-bytes";
    // 48 kHz stereo 24-in-32: 48 frames nominal, 6 bytes per frame - the
    // headroom rule gives 50*6 = 300, above the 288-byte pipe maximum.
    CHECK_EQ(EmuIsochSlotBytes(48, 6, 288), 300);
    // A pipe maximum larger than the nominal packet wins: an IN slot must
    // hold whatever the device actually sends.
    CHECK_EQ(EmuIsochSlotBytes(48, 6, 512), 512);
    // 192 kHz stereo: 192*6 = 1152 nominal, headroom 194*6 = 1164.
    CHECK_EQ(EmuIsochSlotBytes(192, 6, 1152), 1164);
    // Zero packet size cannot happen from a parsed alt, but the function does
    // not depend on that: it still returns at least the pipe maximum.
    CHECK_GE(EmuIsochSlotBytes(0, 6, 288), 288);
}

int main(void)
{
    CasePeriod();
    CaseNominal();
    CaseSlotBytes();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    printf("RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
