// EmuUSBAudio WaveRT audio helper unit test, user-mode.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Build and run: cd driver && ./utest.sh audio
#include <string.h>
#include "testharness.h"
#include "emu_audio.h"

int g_checks = 0;
int g_failures = 0;
const char* g_case = "";

static const UCHAR g_Host[24] = {
    // Frame 0: zero, maximum positive 24-bit sample.
    0x00,0x00,0x00,0x00,  0x00,0xFF,0xFF,0x7F,
    // Frame 1: minimum negative 24-bit sample, -1.
    0x00,0x00,0x00,0x80,  0x00,0xFF,0xFF,0xFF,
    // Frame 2: ignored low bits and a small positive sample.
    0xA5,0x34,0x12,0x7F,  0xCC,0x00,0x01,0x00
};

static const UCHAR g_Usb[18] = {
    0x00,0x00,0x00,  0xFF,0xFF,0x7F,
    0x00,0x00,0x80,  0xFF,0xFF,0xFF,
    0x34,0x12,0x7F,  0x00,0x01,0x00
};

static const UCHAR g_Unpacked[24] = {
    0x00,0x00,0x00,0x00,  0x00,0xFF,0xFF,0x7F,
    0x00,0x00,0x00,0x80,  0x00,0xFF,0xFF,0xFF,
    0x00,0x34,0x12,0x7F,  0x00,0x00,0x01,0x00
};

static void CaseFrameSizes(void)
{
    g_case = "frame sizes";
    CHECK_EQ(EmuAudioHostFrameBytes(1), 4);
    CHECK_EQ(EmuAudioHostFrameBytes(2), 8);
    CHECK_EQ(EmuAudioHostFrameBytes(4), 16);
    CHECK_EQ(EmuAudioUsbFrameBytes(1), 3);
    CHECK_EQ(EmuAudioUsbFrameBytes(2), 6);
    CHECK_EQ(EmuAudioUsbFrameBytes(4), 12);
    CHECK_EQ(EmuAudioHostFrameBytes(0), 0);
    CHECK_EQ(EmuAudioUsbFrameBytes(0), 0);
    CHECK_EQ(EmuAudioHostFrameBytes((ULONG)~0UL), 0);
    CHECK_EQ(EmuAudioUsbFrameBytes((ULONG)~0UL), 0);
}

static void CasePack(void)
{
    UCHAR output[sizeof(g_Usb) + 2];

    g_case = "24-in-32 to packed-24";
    memset(output, 0xA5, sizeof(output));
    CHECK(EmuAudioPack24(output + 1, g_Host, 3, 2));
    CHECK_EQ(output[0], 0xA5);
    CHECK_EQ(output[sizeof(output) - 1], 0xA5);
    CHECK(memcmp(output + 1, g_Usb, sizeof(g_Usb)) == 0);
}

static void CaseUnpack(void)
{
    UCHAR output[sizeof(g_Unpacked) + 2];
    UCHAR roundTrip[sizeof(g_Host)];

    g_case = "packed-24 to 24-in-32";
    memset(output, 0xA5, sizeof(output));
    CHECK(EmuAudioUnpack24(output + 1, g_Usb, 3, 2));
    CHECK_EQ(output[0], 0xA5);
    CHECK_EQ(output[sizeof(output) - 1], 0xA5);
    CHECK(memcmp(output + 1, g_Unpacked, sizeof(g_Unpacked)) == 0);

    memset(roundTrip, 0, sizeof(roundTrip));
    CHECK(EmuAudioPack24(roundTrip, g_Unpacked, 3, 2));
    CHECK(memcmp(roundTrip, g_Usb, sizeof(g_Usb)) == 0);
}

static void CaseConversionRejects(void)
{
    UCHAR bytes[32];

    g_case = "conversion argument rejection";
    memset(bytes, 0x5A, sizeof(bytes));
    CHECK(EmuAudioPack24(NULL, NULL, 0, 2));
    CHECK(EmuAudioUnpack24(NULL, NULL, 0, 2));
    CHECK(!EmuAudioPack24(NULL, g_Host, 1, 2));
    CHECK(!EmuAudioPack24(bytes, NULL, 1, 2));
    CHECK(!EmuAudioUnpack24(NULL, g_Usb, 1, 2));
    CHECK(!EmuAudioUnpack24(bytes, NULL, 1, 2));
    CHECK(!EmuAudioPack24(bytes, g_Host, 1, 0));
    CHECK(!EmuAudioUnpack24(bytes, g_Usb, 1, 0));
    CHECK(!EmuAudioPack24(bytes, g_Host, (SIZE_T)-1, 2));
    CHECK(!EmuAudioUnpack24(bytes, g_Usb, (SIZE_T)-1, 2));
    for (ULONG i = 0; i < sizeof(bytes); i++) {
        CHECK_EQ(bytes[i], 0x5A);
    }
}

static void ResetSpans(EMU_AUDIO_RING_SPAN spans[2], PULONG count)
{
    memset(spans, 0xCC, sizeof(EMU_AUDIO_RING_SPAN) * 2);
    *count = 77;
}

static void CaseRingSpans(void)
{
    EMU_AUDIO_RING_SPAN spans[2];
    ULONG count;

    g_case = "ring spans";
    ResetSpans(spans, &count);
    CHECK(EmuAudioRingSpans(0, 64, 8, 2, spans, &count));
    CHECK_EQ(count, 1);
    CHECK_EQ(spans[0].Offset, 0);
    CHECK_EQ(spans[0].Bytes, 16);
    CHECK_EQ(spans[1].Bytes, 0);

    ResetSpans(spans, &count);
    CHECK(EmuAudioRingSpans(56, 64, 8, 2, spans, &count));
    CHECK_EQ(count, 2);
    CHECK_EQ(spans[0].Offset, 56);
    CHECK_EQ(spans[0].Bytes, 8);
    CHECK_EQ(spans[1].Offset, 0);
    CHECK_EQ(spans[1].Bytes, 8);

    ResetSpans(spans, &count);
    CHECK(EmuAudioRingSpans(120, 64, 8, 2, spans, &count));
    CHECK_EQ(count, 2);
    CHECK_EQ(spans[0].Offset, 56);
    CHECK_EQ(spans[1].Offset, 0);

    // A request may exceed the ring; the producer consumes the two spans in
    // chunks rather than rejecting a large client buffer.
    ResetSpans(spans, &count);
    CHECK(EmuAudioRingSpans(56, 64, 8, 4, spans, &count));
    CHECK_EQ(count, 2);
    CHECK_EQ(spans[0].Bytes, 8);
    CHECK_EQ(spans[1].Bytes, 24);

    ResetSpans(spans, &count);
    CHECK(EmuAudioRingSpans(64, 64, 8, 0, spans, &count));
    CHECK_EQ(count, 0);
    CHECK_EQ(spans[0].Bytes, 0);
    CHECK_EQ(spans[1].Bytes, 0);
}

static void CaseRingRejects(void)
{
    EMU_AUDIO_RING_SPAN spans[2];
    ULONG count;

    g_case = "ring argument rejection";
    ResetSpans(spans, &count);
    CHECK(!EmuAudioRingSpans(0, 0, 8, 1, spans, &count));
    CHECK(!EmuAudioRingSpans(0, 64, 0, 1, spans, &count));
    CHECK(!EmuAudioRingSpans(0, 60, 8, 1, spans, &count));
    CHECK(!EmuAudioRingSpans(4, 64, 8, 1, spans, &count));
    CHECK(!EmuAudioRingSpans(0, 64, 8, ((ULONG)~0UL) / 4, spans, &count));
    CHECK(!EmuAudioRingSpans(0, 64, 8, 1, NULL, &count));
    CHECK(!EmuAudioRingSpans(0, 64, 8, 1, spans, NULL));
    CHECK_EQ(count, 77);
}

static void CasePositions(void)
{
    ULONGLONG linear;
    ULONG ring;

    g_case = "linear and ring positions";
    linear = 99;
    ring = 99;
    CHECK(EmuAudioPositions(0, 64, 8, &linear, &ring));
    CHECK_EQ(linear, 0);
    CHECK_EQ(ring, 0);
    CHECK(EmuAudioPositions(1, 64, 8, &linear, &ring));
    CHECK_EQ(linear, 8);
    CHECK_EQ(ring, 8);
    CHECK(EmuAudioPositions(48, 64, 8, &linear, &ring));
    CHECK_EQ(linear, 384);
    CHECK_EQ(ring, 0);
    CHECK(EmuAudioPositions(49, 64, 8, &linear, &ring));
    CHECK_EQ(linear, 392);
    CHECK_EQ(ring, 8);
    CHECK(EmuAudioPositions(0x100000001ULL, 64, 8, &linear, &ring));
    CHECK_EQ(linear, 0x800000008ULL);
    CHECK_EQ(ring, 8);
}

static void CasePositionRejects(void)
{
    ULONGLONG linear;
    ULONG ring;

    g_case = "position argument rejection";
    linear = 0x1234;
    ring = 0x56;
    CHECK(!EmuAudioPositions(0, 0, 8, &linear, &ring));
    CHECK(!EmuAudioPositions(0, 64, 0, &linear, &ring));
    CHECK(!EmuAudioPositions(0, 60, 8, &linear, &ring));
    CHECK(!EmuAudioPositions(0, 64, 8, NULL, &ring));
    CHECK(!EmuAudioPositions(0, 64, 8, &linear, NULL));
    CHECK(!EmuAudioPositions(~(ULONGLONG)0, 64, 8, &linear, &ring));
    CHECK_EQ(linear, 0x1234);
    CHECK_EQ(ring, 0x56);
}

int main(void)
{
    CaseFrameSizes();
    CasePack();
    CaseUnpack();
    CaseConversionRejects();
    CaseRingSpans();
    CaseRingRejects();
    CasePositions();
    CasePositionRejects();
    return TestResult();
}
