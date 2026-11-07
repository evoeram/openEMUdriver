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
    // this heuristic assumes Interval counts microframes (8 = 1 ms). The
    // device's PipeInformation.Interval is in fact the RAW descriptor
    // bInterval — the 0404's 1 ms grid reads 4 there — so the real pipe lands
    // on the full-speed reading below. The engine treats PeriodMs as a
    // breadcrumb-only value and schedules on its hardcoded vendor-trace grid;
    // this function stays pinned as written so its behavior cannot drift
    // silently.
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

static void CaseCursorAnchor(void)
{
    ULONG cursor;

    g_case = "cursor-anchor";
    // Behind the bus: the vendor's 0x4DBE invariant re-anchors to now + 3.
    cursor = 100;
    CHECK_EQ(EmuIsochCursorAnchor(110, &cursor), 1);
    CHECK_EQ(cursor, 113);
    // Exactly at the bus frame counts as behind: a StartFrame on the current
    // frame would be scheduled into a service interval that already started.
    cursor = 110;
    CHECK_EQ(EmuIsochCursorAnchor(110, &cursor), 1);
    CHECK_EQ(cursor, 113);
    // A healthy lead stands: the 8-URB pipeline (8 frames) plus the feedback
    // step (32) never comes near the ceiling.
    cursor = 121;
    CHECK_EQ(EmuIsochCursorAnchor(110, &cursor), 0);
    CHECK_EQ(cursor, 121);
    // Just under the ceiling stands too.
    cursor = 110 + EMU_ISOCH_MAX_LEAD;
    CHECK_EQ(EmuIsochCursorAnchor(110, &cursor), 0);
    CHECK_EQ(cursor, 110 + EMU_ISOCH_MAX_LEAD);
    // One frame past the ceiling is clamped back to now + 3: the runaway
    // kill-switch. The 0x133 dump had the feedback cursor ~66 h ahead of the
    // bus after ~7.5 million same-DPC re-arm cycles; without the ceiling the
    // vendor invariant (which only corrects a cursor BEHIND the bus) can
    // never fire again once the storm starts.
    cursor = 110 + EMU_ISOCH_MAX_LEAD + 1;
    CHECK_EQ(EmuIsochCursorAnchor(110, &cursor), 2);
    CHECK_EQ(cursor, 113);
    // ULONG wrap-around: the comparison is signed on the difference, exactly
    // as the pump's original inline version did. A cursor just below the wrap
    // with the bus already past it is genuinely behind (signed lead -13) and
    // re-anchors; a cursor just past the wrap with the bus still below it is
    // genuinely 21 frames ahead and stands.
    cursor = 0xFFFFFFF8;
    CHECK_EQ(EmuIsochCursorAnchor(5, &cursor), 1);
    CHECK_EQ(cursor, 5 + EMU_ISOCH_FRAME_MARGIN);
    cursor = 5;
    CHECK_EQ(EmuIsochCursorAnchor(0xFFFFFFF0, &cursor), 0);
    CHECK_EQ(cursor, 5);
}

static void CaseFramesPerMs(void)
{
    g_case = "frames-per-ms";
    // The two counter shapes the stack reports: millisecond frames (USB2) and
    // the microframe-shaped counter (USB3, 8 per millisecond).
    CHECK(EmuIsochFramesPerMsValid(1));
    CHECK(EmuIsochFramesPerMsValid(8));
    CHECK(EmuIsochFramesPerMsValid(16));
    // A frozen counter (delta 0 over the window) and any foreign unit are
    // refusals: anchoring StartFrames from them schedules every packet into
    // the past or the far future - the runaway class. The start path fails
    // loudly instead of silently mapping 0 to 1 as it used to.
    CHECK(!EmuIsochFramesPerMsValid(0));
    CHECK(!EmuIsochFramesPerMsValid(17));
    CHECK(!EmuIsochFramesPerMsValid(1000));
}

static void CasePickAlt(void)
{
    static EMU_STREAM_ALT alts[2];

    g_case = "pick-alt";
    // The 0404 lists every rate twice, in descriptor order: first the 1 ms
    // service-interval alt (raw bInterval 4, MPS = frames/ms*6+10), then the
    // 500 us one (raw bInterval 3, MPS = frames-per-500us*6+10). The real
    // 2ch/48k pair is alt 3 (MPS 298) and alt 4 (MPS 154 = 24*6+10) — and the
    // vendor's 2-packets-per-1ms model is only legal on alt 4.
    alts[0].Direction = EMU_STREAM_RENDER;  alts[0].Channels = 2;
    alts[0].AltSetting = 3;                 alts[0].DataInterval = 4;
    alts[0].DataMaxPacket = 298;
    alts[0].RateCount = 1;                  alts[0].Rates[0] = 48000;
    alts[1].Direction = EMU_STREAM_RENDER;  alts[1].Channels = 2;
    alts[1].AltSetting = 4;                 alts[1].DataInterval = 3;
    alts[1].DataMaxPacket = 154;
    alts[1].RateCount = 1;                  alts[1].Rates[0] = 48000;
    // The grid match wins over descriptor order: alt 4, not alt 3.
    CHECK_EQ(EmuIsochPickAltIndex(alts, 2, EMU_STREAM_RENDER, 2, 48000), 1);
    // A rate only the 1 ms alt lists: fall back to the first match.
    alts[0].Rates[1] = 96000;  alts[0].RateCount = 2;
    CHECK_EQ(EmuIsochPickAltIndex(alts, 2, EMU_STREAM_RENDER, 2, 96000), 0);
    // Nothing matches: Count comes back, the caller falls back.
    CHECK_EQ(EmuIsochPickAltIndex(alts, 2, EMU_STREAM_RENDER, 2, 44100), 2);
    // A device listing only 1 ms alts: first match, the preference never fires.
    alts[1].DataInterval = 4;
    CHECK_EQ(EmuIsochPickAltIndex(alts, 2, EMU_STREAM_RENDER, 2, 48000), 0);
    // Direction and channel filters still apply.
    CHECK_EQ(EmuIsochPickAltIndex(alts, 2, EMU_STREAM_CAPTURE, 2, 48000), 2);
    CHECK_EQ(EmuIsochPickAltIndex(alts, 2, EMU_STREAM_RENDER, 1, 48000), 2);
}

int main(void)
{
    CasePeriod();
    CaseNominal();
    CaseSlotBytes();
    CaseCursorAnchor();
    CaseFramesPerMs();
    CasePickAlt();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    printf("RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
