// EmuUSBAudio settings-cache unit test, user-mode.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Build and run:  cd driver && ./utest.sh state
//
// The cache is judged against a descriptor that was actually parsed, not against
// hand-filled structures: what the driver refuses to believe is decided by the
// rate lists the parser produced, so the fixture in tests/descfixture.h is part
// of the assertion.
#include "descfixture.h"
#include "emu_state.h"
#include "testharness.h"

int g_checks = 0;
int g_failures = 0;
const char* g_case = "";

static void CaseInitUnknown(void)
{
    g_case = "init-unknown";
    EMU_SETTINGS s;

    // Fill with plausible values first: init has to overwrite all of them, and
    // a field it misses would look like a control the driver confirmed.
    s.RateHz = 48000;
    s.ClockSource = 0;
    s.SpdifFormat = 1;
    s.SoftLimit = 1;
    EmuSettingsInit(&s);

    CHECK_EQ(s.RateHz, EMU_NO_VALUE);
    CHECK_EQ(s.ClockSource, EMU_NO_VALUE);
    CHECK_EQ(s.SpdifFormat, EMU_NO_VALUE);
    CHECK_EQ(s.SoftLimit, EMU_NO_VALUE);
    // The point of the marker: "unknown" must not read as the legal value zero.
    CHECK(s.RateHz != 0u);
    CHECK(s.ClockSource != 0u);
    CHECK(s.SoftLimit != 0u);
}

static void CaseNoteRate(void)
{
    g_case = "note-rate";
    static EMU_DEVICE_CAPS caps;
    EMU_SETTINGS s;
    CHECK(Parse0404(&caps));
    EmuSettingsInit(&s);

    // Every rate the 0404 fixture lists somewhere is cacheable, including 192
    // kHz, which only one of the three alternate settings carries.
    CHECK(EmuSettingsNoteRate(&s, &caps, 44100));
    CHECK_EQ(s.RateHz, 44100);
    CHECK(EmuSettingsNoteRate(&s, &caps, 48000));
    CHECK_EQ(s.RateHz, 48000);
    CHECK(EmuSettingsNoteRate(&s, &caps, 88200));
    CHECK(EmuSettingsNoteRate(&s, &caps, 96000));
    CHECK(EmuSettingsNoteRate(&s, &caps, 176400));
    CHECK(EmuSettingsNoteRate(&s, &caps, 192000));
    CHECK_EQ(s.RateHz, 192000);
    // One note does not turn the other controls into known values.
    CHECK_EQ(s.ClockSource, EMU_NO_VALUE);
    CHECK_EQ(s.SpdifFormat, EMU_NO_VALUE);
    CHECK_EQ(s.SoftLimit, EMU_NO_VALUE);

    // Rejected values leave the known rate in place. A clock-rate GET that
    // stalls halfways or answers with an index outside the table decodes to a
    // rate of 0; that must not erase what the driver already believes.
    CHECK(!EmuSettingsNoteRate(&s, &caps, 0));
    CHECK_EQ(s.RateHz, 192000);
    CHECK(!EmuSettingsNoteRate(&s, &caps, 8000));
    CHECK(!EmuSettingsNoteRate(&s, &caps, 44101));
    CHECK(!EmuSettingsNoteRate(&s, &caps, 192001));
    CHECK(!EmuSettingsNoteRate(&s, &caps, 384000));
    CHECK_EQ(s.RateHz, 192000);

    // Unknown stays unknown: refusing is not the same as recording zero.
    EmuSettingsInit(&s);
    CHECK(!EmuSettingsNoteRate(&s, &caps, 99999));
    CHECK_EQ(s.RateHz, EMU_NO_VALUE);
}

// A descriptor that advertises a continuous range rather than a list is the
// common UAC1 alternative, and the cache has to follow it.
static void CaseNoteRateContinuous(void)
{
    g_case = "note-rate-continuous";
    Block b;
    static EMU_DEVICE_CAPS caps;
    EMU_SETTINGS s;

    ConfigStart(b);
    AcInterface(b, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(b, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIContinuous(b, 2, 3, 24, 8000, 192000);
    Endpoint(b, 0x01, 0x01, 192, 1);
    ConfigFinish(b, 2);
    CheckStatus("continuous-parse", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &caps));

    EmuSettingsInit(&s);
    CHECK(EmuSettingsNoteRate(&s, &caps, 8000));
    CHECK(EmuSettingsNoteRate(&s, &caps, 44101));      // inside the range, not a listed rate
    CHECK(EmuSettingsNoteRate(&s, &caps, 192000));     // the maximum is included
    CHECK(!EmuSettingsNoteRate(&s, &caps, 7999));
    CHECK(!EmuSettingsNoteRate(&s, &caps, 192001));
    CHECK(!EmuSettingsNoteRate(&s, &caps, 0));
    CHECK_EQ(s.RateHz, 192000);
}

// A table with no alternate settings contradicts everything. This is what Caps
// looks like after a failed parse, and a cache built on it stays unknown.
static void CaseNoteRateWithoutCaps(void)
{
    g_case = "note-rate-empty-caps";
    static EMU_DEVICE_CAPS caps;
    EMU_SETTINGS s;
    Block b;
    NTSTATUS status;

    b.assign(Emu0404RealDescriptor,
             Emu0404RealDescriptor + sizeof(Emu0404RealDescriptor));
    b[b.size() - 5] = 0;                     // last descriptor claims bLength 0
    status = EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &caps);
    CheckStatus("parse-fails", STATUS_DATA_ERROR, status);

    EmuSettingsInit(&s);
    CHECK_EQ(caps.StreamAltCount, 0u);
    CHECK(!EmuSettingsNoteRate(&s, &caps, 48000));
    CHECK(!EmuSettingsNoteRate(&s, &caps, 44100));
    CHECK_EQ(s.RateHz, EMU_NO_VALUE);
}

// The sequence is what makes a change visible to a reader that polls: it counts
// the notes the cache accepted, and nothing else moves it.
static void CaseSequenceCountsAcceptedNotes(void)
{
    g_case = "sequence";
    static EMU_DEVICE_CAPS caps;
    EMU_SETTINGS s;

    CHECK(Parse0404(&caps));
    EmuSettingsInit(&s);
    CHECK_EQ(s.Sequence, 0u);

    // Only accepted notes count, and a note that repeats the rate already cached
    // counts too: the reader compares the sequence against the control it
    // re-read, so what matters is that something was confirmed, not that the
    // number changed.
    CHECK(EmuSettingsNoteRate(&s, &caps, 48000));
    CHECK_EQ(s.Sequence, 1u);
    CHECK(EmuSettingsNoteRate(&s, &caps, 48000));
    CHECK_EQ(s.Sequence, 2u);
    CHECK(EmuSettingsNoteRate(&s, &caps, 96000));
    CHECK_EQ(s.Sequence, 3u);
    CHECK_EQ(s.RateHz, 96000u);

    // Refused values are not evidence, so they must not look like a change.
    CHECK(!EmuSettingsNoteRate(&s, &caps, 44101));
    CHECK_EQ(s.Sequence, 3u);
    CHECK(!EmuSettingsNoteRate(&s, &caps, 0));
    CHECK_EQ(s.Sequence, 3u);
    CHECK(!EmuSettingsNoteRate(&s, &caps, 192001));
    CHECK_EQ(s.RateHz, 96000u);

    EmuSettingsInit(&s);
    CHECK_EQ(s.Sequence, 0u);
}

int
main(void)
{
    printf("openEMUdriver EmuUSBAudio settings cache test\n");
    printf("(cache: driver/emuusbaudio/Emu/emu_state.cpp, same source as in the driver)\n");

    CaseInitUnknown();
    CaseNoteRate();
    CaseNoteRateContinuous();
    CaseNoteRateWithoutCaps();
    CaseSequenceCountsAcceptedNotes();

    return TestResult();
}
