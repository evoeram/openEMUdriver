// EmuUSBAudio descriptor-parser unit test (plan test #1), user-mode.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Build and run:  cd driver && ./utest.sh
//
// The fixture builders live in tests/descfixture.h. The baseline there is
// RECONSTRUCTED from the measurements recorded in docs/RE_NOTES.md ("all audio
// interfaces are declared vendor-class 0xFF but carry the standard UAC1 layout",
// OUT/IN 2ch 44.1-192 kHz plus 4ch 44.1-96 kHz alternate settings, 24 bit in
// 3-byte slots, render data EP 0x00 with feedback EP 0x81 (4 bytes), capture
// EP 0x82 with implicit-feedback usage, AC interrupt EP 0x83 (2 bytes), XU codes
// 0xE301..0xE306). It is not a live capture. Once `emu-dump -o docs/descriptors`
// runs against the hardware the raw hex block there replaces this builder, and
// the byte-for-byte assertions can be tightened.
#include "descfixture.h"
#include "testharness.h"

int g_checks = 0;
int g_failures = 0;
const char* g_case = "";

// ---------------------------------------------------------------------------
// cases
// ---------------------------------------------------------------------------

static void
DumpCaps(_In_ const char* what, _In_ const EMU_DEVICE_CAPS* caps)
{
    printf("    caps [%s]: flags=0x%X ac=%lu status=0x%lX ifaces=%lu alts=%lu xu=%lu feat=%lu\n",
           what, caps->ParsedFlags, caps->AcInterfaceNumber, caps->StatusEndpoint,
           caps->AudioInterfaceCount, caps->StreamAltCount, caps->XuUnitCount,
           caps->FeatureUnitCount);
    for (ULONG i = 0; i < caps->AudioInterfaceCount; i++) {
        printf("      iface %2lu class=0x%02X subclass=0x%02X role=%lu\n",
               caps->AudioInterfaces[i].InterfaceNumber,
               caps->AudioInterfaces[i].DeclaredClass,
               caps->AudioInterfaces[i].DeclaredSubclass,
               caps->AudioInterfaces[i].Role);
    }
    for (ULONG i = 0; i < caps->StreamAltCount; i++) {
        printf("      alt iface=%lu alt=%lu dir=%lu ch=%lu rates=%lu data=0x%lX fb=0x%lX\n",
               caps->StreamAlts[i].InterfaceNumber, caps->StreamAlts[i].AltSetting,
               caps->StreamAlts[i].Direction, caps->StreamAlts[i].Channels,
               caps->StreamAlts[i].RateCount, caps->StreamAlts[i].DataEndpoint,
               caps->StreamAlts[i].FeedbackEndpoint);
    }
}

static void Run(_In_ const char* name, _Inout_ Block& b, _In_ NTSTATUS expected)
{
    g_case = name;
    static EMU_DEVICE_CAPS caps;
    NTSTATUS status = EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &caps);
    CheckStatus(name, expected, status);
    if (status != expected) {
        DumpCaps(name, &caps);
    }
}

// Full parse of the captured 0404 descriptor, field by field. The expectations
// are the live device's facts: 18 render plus 16 capture alts, roles declared
// by protocol with the subclass hidden, four E-MU XUs with codes 0xE301..0xE304
// (direct monitor and metering do not exist on this firmware), a master-mute
// feature unit, and a MIDI interface whose descriptors must not disturb the
// audio parse.
static void CaseBaseline0404(void)
{
    g_case = "baseline-0404";

    static EMU_DEVICE_CAPS caps;
    if (!Parse0404(&caps)) {
        CHECK(FALSE && "Parse0404 failed");
        return;
    }

    CHECK_EQ(caps.DeclaredTotalLength, 1832u);
    CHECK_EQ(caps.BufferLength, 1832u);
    CHECK_EQ(caps.ConfigurationValue, 1);
    CHECK(caps.ParsedFlags & EMU_CAPS_AC_FOUND);
    CHECK(caps.ParsedFlags & EMU_CAPS_RENDER);
    CHECK(caps.ParsedFlags & EMU_CAPS_CAPTURE);
    CHECK(caps.ParsedFlags & EMU_CAPS_STATUS_ENDPOINT);
    CHECK(caps.ParsedFlags & EMU_CAPS_HAS_XU);
    CHECK(caps.ParsedFlags & EMU_CAPS_MIDI_INTERFACE);
    // The firmware hides the class byte (0xFF) behind standard subclasses —
    // exactly what RE_NOTES recorded and what the INF binding story assumes.
    CHECK(caps.ParsedFlags & EMU_CAPS_VENDOR_CLASS);
    // The 16-bit render alts are FORMAT_TYPE_III on this firmware: recorded as
    // non-PCM (this build streams 24-in-32 Type I only), never an error.
    CHECK(caps.ParsedFlags & EMU_CAPS_NON_PCM_FORMAT);

    CHECK_EQ(caps.AcInterfaceNumber, 0);
    CHECK_EQ(caps.StatusEndpoint, 0x83);
    CHECK_EQ(caps.AudioInterfaceCount, 4);
    CHECK_EQ(caps.AudioInterfaces[0].Role, EMU_ROLE_AC);
    CHECK_EQ(caps.AudioInterfaces[0].StatusMaxPacket, 2);
    CHECK_EQ(caps.AudioInterfaces[0].StatusInterval, 10);
    CHECK_EQ(caps.AudioInterfaces[1].Role, EMU_ROLE_AS);
    CHECK_EQ(caps.AudioInterfaces[2].Role, EMU_ROLE_AS);
    CHECK_EQ(caps.AudioInterfaces[3].Role, EMU_ROLE_MIDI);

    // alt 0 of every streaming interface must not be recorded; the MIDI
    // interface contributes no alts; the two 16-bit Type III alts are skipped
    // as non-PCM. 16 render + 16 capture remain.
    CHECK_EQ(caps.StreamAltCount, 32u);
    CHECK_EQ(EmuCapsAltCount(&caps, EMU_STREAM_RENDER), 16u);
    CHECK_EQ(EmuCapsAltCount(&caps, EMU_STREAM_CAPTURE), 16u);

    PEMU_STREAM_ALT r1 = &caps.StreamAlts[0];
    CHECK_EQ(r1->InterfaceNumber, 1);
    CHECK_EQ(r1->AltSetting, 1);
    CHECK_EQ(r1->Direction, EMU_STREAM_RENDER);
    CHECK_EQ(r1->TerminalId, 0x01);
    CHECK_EQ(r1->FormatTag, 1);
    CHECK_EQ(r1->Channels, 2);
    CHECK_EQ(r1->SubFrameSize, 3);
    CHECK_EQ(r1->BitResolution, 24);
    CHECK_EQ(r1->RateCount, 1);
    CHECK_EQ(r1->Rates[0], 44100);
    CHECK_EQ(r1->DataEndpoint, 0x01);
    CHECK_EQ(r1->DataMaxPacket, 274);
    CHECK_EQ(r1->DataUsageType, EMU_EP_USAGE_DATA);
    CHECK_EQ(r1->FeedbackEndpoint, 0x81);
    CHECK_EQ(r1->FeedbackMaxPacket, 4);
    CHECK_EQ(r1->RateFlags, 0);

    // The device doubles every rate into a high- and a low-bandwidth alt; the
    // second 44.1k alt carries the smaller packet bound.
    PEMU_STREAM_ALT r2 = &caps.StreamAlts[1];
    CHECK_EQ(r2->AltSetting, 2);
    CHECK_EQ(r2->Channels, 2);
    CHECK_EQ(r2->Rates[0], 44100);
    CHECK_EQ(r2->DataMaxPacket, 142);
    CHECK_EQ(r2->FeedbackEndpoint, 0x81);

    // 176.4 and 192 kHz exist only as 2ch/24-bit; the highest 4ch rate is 96k.
    PEMU_STREAM_ALT r192 = &caps.StreamAlts[9];
    CHECK_EQ(r192->AltSetting, 10);
    CHECK_EQ(r192->Rates[0], 192000);
    PEMU_STREAM_ALT r4 = &caps.StreamAlts[10];
    CHECK_EQ(r4->AltSetting, 11);
    CHECK_EQ(r4->Channels, 4);
    CHECK_EQ(r4->Rates[0], 44100);

    // Alts 17/18 (the 16-bit Type III pair) are non-PCM skips, so the capture
    // table starts right after the render one.
    PEMU_STREAM_ALT c1 = &caps.StreamAlts[16];
    CHECK_EQ(c1->InterfaceNumber, 2);
    CHECK_EQ(c1->AltSetting, 1);
    CHECK_EQ(c1->Direction, EMU_STREAM_CAPTURE);

    CHECK_EQ(c1->TerminalId, 0x0A);
    CHECK_EQ(c1->DataEndpoint, 0x82);
    CHECK_EQ(c1->DataUsageType, EMU_EP_USAGE_IMPLICIT_DATA);
    CHECK_EQ(c1->FeedbackEndpoint, EMU_NO_VALUE);

    CHECK_EQ(caps.MaxChannels, 4);
    CHECK_EQ(caps.MinRateHz, 44100);
    CHECK_EQ(caps.MaxRateHz, 192000);

    CHECK_EQ(caps.XuUnitCount, 4);
    CHECK_EQ(EmuCapsXuUnitId(&caps, EMU_XU_CLOCK_RATE), 0x0C);
    CHECK_EQ(EmuCapsXuUnitId(&caps, EMU_XU_CLOCK_SOURCE), 0x0D);
    CHECK_EQ(EmuCapsXuUnitId(&caps, EMU_XU_DIGITAL_IO), 0x0E);
    CHECK_EQ(EmuCapsXuUnitId(&caps, EMU_XU_DEVICE_OPTIONS), 0x0F);
    CHECK_EQ(EmuCapsXuUnitId(&caps, EMU_XU_DIRECT_MONITOR), EMU_NO_VALUE);
    CHECK_EQ(EmuCapsXuUnitId(&caps, EMU_XU_METERING), EMU_NO_VALUE);
    CHECK_EQ(EmuCapsXuUnitId(&caps, 0xE3FF), EMU_NO_VALUE);
    // In descriptor order, not code order: 0x0E carries 0xE303, and the clock
    // pair is behind it. EmuCapsXuUnitId finds by code regardless.
    CHECK_EQ(caps.XuUnits[0].UnitID, 0x0E);
    CHECK_EQ(caps.XuUnits[0].ExtensionCode, EMU_XU_DIGITAL_IO);
    CHECK_EQ(caps.XuUnits[1].UnitID, 0x0C);
    CHECK_EQ(caps.XuUnits[1].ExtensionCode, EMU_XU_CLOCK_RATE);
    CHECK_EQ(caps.XuUnits[2].UnitID, 0x0D);
    CHECK_EQ(caps.XuUnits[2].ExtensionCode, EMU_XU_CLOCK_SOURCE);
    CHECK_EQ(caps.XuUnits[3].UnitID, 0x0F);
    CHECK_EQ(caps.XuUnits[3].ExtensionCode, EMU_XU_DEVICE_OPTIONS);
    for (ULONG i = 0; i < caps.XuUnitCount; i++) {
        CHECK_EQ(caps.XuUnits[i].Layout, EMU_XU_LAYOUT_EMU);
        CHECK_EQ(caps.XuUnits[i].InterfaceNumber, 0);
    }

    CHECK_EQ(caps.FeatureUnitCount, 1);
    CHECK_EQ(caps.FeatureUnits[0].UnitID, 0x04);
    CHECK_EQ(caps.FeatureUnits[0].SourceID, 0x01);
    CHECK_EQ(caps.FeatureUnits[0].ControlSize, 2);
    CHECK_EQ(caps.FeatureUnits[0].ChannelCount, 4);
    // The master control is mute only; volume lives on the four channels.
    CHECK_EQ(caps.FeatureUnits[0].MasterControls, 0x01);
}

// Standard-class declaration (bInterfaceClass 0x01) must parse identically —
// the parser keys on subclass plus class-specific content, not on 0xFF.
static void CaseStandardClassDeclaration(void)
{
    g_case = "standard-class";
    Block b;
    ConfigStart(b);
    AcInterface(b, 0, EMU_INTERFACE_CLASS_AUDIO);
    Interface(b, 1, 0, 0, EMU_INTERFACE_CLASS_AUDIO, 0x02, 0x00);
    Interface(b, 1, 1, 1, EMU_INTERFACE_CLASS_AUDIO, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIDiscrete(b, 2, 3, 24, {48000});
    Endpoint(b, 0x01, 0x01, 192, 1);
    ConfigFinish(b, 2);

    static EMU_DEVICE_CAPS caps;
    CheckStatus("standard-class", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &caps));
    CHECK_EQ(caps.StreamAltCount, 1);
    CHECK_EQ(caps.StreamAlts[0].Direction, EMU_STREAM_RENDER);
    CHECK_EQ(caps.ParsedFlags & EMU_CAPS_VENDOR_CLASS, 0);
}

// Class-spec EXTENSION_UNIT layout, and the E-MU layout, in one device.
static void CaseXuBothLayouts(void)
{
    g_case = "xu-both-layouts";
    Block b;
    ConfigStart(b);
    Interface(b, 0, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x01, 0x00);
    AcHeader(b);
    InputTerminal(b, 0x03, 0x0301, 2);
    XuSpec(b, 0x20, 0x0B00);          // spec layout, non-E-MU code
    XuEmu(b, 0x21, EMU_XU_CLOCK_RATE);
    Interface(b, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(b, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIDiscrete(b, 2, 3, 24, {48000});
    Endpoint(b, 0x01, 0x01, 192, 1);
    ConfigFinish(b, 2);

    static EMU_DEVICE_CAPS caps;
    CheckStatus("xu-both-layouts", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &caps));
    CHECK_EQ(caps.XuUnitCount, 2);
    CHECK_EQ(caps.XuUnits[0].Layout, EMU_XU_LAYOUT_SPEC);
    CHECK_EQ(caps.XuUnits[0].ExtensionCode, 0x0B00);
    CHECK_EQ(caps.XuUnits[1].Layout, EMU_XU_LAYOUT_EMU);
    CHECK_EQ(EmuCapsXuUnitId(&caps, EMU_XU_CLOCK_RATE), 0x21);
}

static void CaseContinuousRates(void)
{
    g_case = "continuous-rates";
    Block b;
    ConfigStart(b);
    AcInterface(b, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(b, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(b, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIContinuous(b, 2, 3, 24, 8000, 192000);
    Endpoint(b, 0x01, 0x01, 192, 1);
    ConfigFinish(b, 2);

    static EMU_DEVICE_CAPS caps;
    CheckStatus("continuous-rates", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &caps));
    CHECK_EQ(caps.StreamAltCount, 1);
    CHECK_EQ(caps.StreamAlts[0].RateFlags & EMU_ALT_CONTINUOUS_RATES,
             EMU_ALT_CONTINUOUS_RATES);
    CHECK_EQ(caps.StreamAlts[0].RateCount, 0);
    CHECK_EQ(caps.StreamAlts[0].ContinuousMinHz, 8000);
    CHECK_EQ(caps.StreamAlts[0].ContinuousMaxHz, 192000);
    CHECK_EQ(caps.MinRateHz, 8000);
    CHECK_EQ(caps.MaxRateHz, 192000);
}

// A non-PCM format tag is recorded as a flag and the alt is dropped; a device
// whose only stream is non-PCM has nothing usable and must fail outright.
static void CaseNonPcmAltDropped(void)
{
    Block b;
    ConfigStart(b);
    AcInterface(b, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(b, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(b, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x03, 0x0102);            // not PCM
    FormatTypeIDiscrete(b, 2, 3, 24, {48000});
    Endpoint(b, 0x01, 0x01, 192, 1);
    ConfigFinish(b, 2);
    Run("non-pcm-only", b, STATUS_DEVICE_CONFIGURATION_ERROR);

    g_case = "non-pcm-mixed";
    Block c;
    ConfigStart(c);
    AcInterface(c, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(c, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(c, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(c, 0x03, 0x0102);
    FormatTypeIDiscrete(c, 2, 3, 24, {48000});
    Endpoint(c, 0x01, 0x01, 192, 1);
    Interface(c, 1, 2, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(c, 0x03, 1);
    FormatTypeIDiscrete(c, 2, 3, 24, {48000});
    Endpoint(c, 0x02, 0x01, 192, 1);
    ConfigFinish(c, 2);

    static EMU_DEVICE_CAPS caps;
    CheckStatus("non-pcm-mixed", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(c.data(), (ULONG)c.size(), &caps));
    CHECK_EQ(caps.StreamAltCount, 1);
    CHECK_EQ(caps.StreamAlts[0].AltSetting, 2);
    CHECK(caps.ParsedFlags & EMU_CAPS_NON_PCM_FORMAT);
}

// A device that emits an alternate setting's endpoints before its class-specific
// descriptors still parses: the commit happens at the interface boundary, not at
// the first format descriptor.
static void CaseEndpointOrderReversed(void)
{
    g_case = "endpoint-order-reversed";
    Block b;
    ConfigStart(b);
    AcInterface(b, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(b, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(b, 1, 1, 2, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Endpoint(b, 0x01, 0x01, 192, 1);
    Endpoint(b, 0x81, 0x11, 4, 1);
    AsGeneral(b, 0x03, 1);
    FormatTypeIDiscrete(b, 2, 3, 24, {48000});
    ConfigFinish(b, 2);

    static EMU_DEVICE_CAPS caps;
    CheckStatus("endpoint-order-reversed", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &caps));
    CHECK_EQ(caps.StreamAltCount, 1);
    CHECK_EQ(caps.StreamAlts[0].DataEndpoint, 0x01);
    CHECK_EQ(caps.StreamAlts[0].FeedbackEndpoint, 0x81);
    CHECK_EQ(caps.StreamAlts[0].RateCount, 1);
}

static void CaseXuMalformed(void)
{
    Block b;
    ConfigStart(b);
    Interface(b, 0, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x01, 0x00);
    AcHeader(b);
    InputTerminal(b, 0x03, 0x0301, 2);
    Bytes(b, {8, 0x24, 0x08, 0x30, 0x00, 0x00, 0x11, 0x22});   // bNrSources 0, no E-MU code
    Interface(b, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIDiscrete(b, 2, 3, 24, {48000});
    Endpoint(b, 0x01, 0x01, 192, 1);
    ConfigFinish(b, 2);
    Run("xu-neither-layout", b, STATUS_DATA_ERROR);

    Block short_xu;
    ConfigStart(short_xu);
    Interface(short_xu, 0, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x01, 0x00);
    AcHeader(short_xu);
    InputTerminal(short_xu, 0x03, 0x0301, 2);
    Bytes(short_xu, {5, 0x24, 0x08, 0x30, 0x01});     // cannot hold a unit ID + code
    Interface(short_xu, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(short_xu, 0x03, 1);
    FormatTypeIDiscrete(short_xu, 2, 3, 24, {48000});
    Endpoint(short_xu, 0x01, 0x01, 192, 1);
    ConfigFinish(short_xu, 2);
    Run("xu-too-short", short_xu, STATUS_DATA_ERROR);
}

// ---------------------------------------------------------------------------
// malformed input
// ---------------------------------------------------------------------------

static void CaseMalformed(void)
{
    Block good;
    ConfigStart(good);
    AcInterface(good, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(good, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(good, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(good, 0x03, 1);
    FormatTypeIDiscrete(good, 2, 3, 24, {48000});
    Endpoint(good, 0x01, 0x01, 192, 1);
    ConfigFinish(good, 2);
    Run("control-good", good, STATUS_SUCCESS);

    Block b = good;     // bLength runs past the block
    b[b.size() - 7] = 40;
    Run("descriptor-overruns-block", b, STATUS_DATA_ERROR);

    b = good;           // bLength of the last descriptor is 1 (below the 2-byte header)
    b[b.size() - 7] = 1;
    Run("descriptor-length-one", b, STATUS_DATA_ERROR);

    b = good;           // bLength zero: would spin the walk forever
    b[b.size() - 7] = 0;
    Run("descriptor-length-zero", b, STATUS_DATA_ERROR);

    b = good;           // block does not end on a descriptor boundary
    b.push_back(0x00);
    b[2] = (unsigned char)(b.size() & 0xFF);
    b[3] = (unsigned char)(b.size() >> 8);
    Run("ragged-tail", b, STATUS_DATA_ERROR);

    b = good;           // wTotalLength disagrees with the bytes handed over
    b[2] = 0x00;
    b[3] = 0x01;
    Run("total-length-mismatch", b, STATUS_DATA_ERROR);

    b = good;           // not a configuration descriptor at all
    b[1] = 0x07;
    Run("wrong-descriptor-type", b, STATUS_DATA_ERROR);

    b = good;           // endpoint shorter than 7 bytes
    b[b.size() - 7] = 6;
    Run("endpoint-too-short", b, STATUS_DATA_ERROR);

    b = good;           // FORMAT_TYPE_I claiming more rates than the descriptor holds
    ULONG ft = (ULONG)b.size() - 7 - 11;
    b[ft + 7] = 4;                       // bSamFreqType = 4 rates, only 1 present
    Run("format-type-rate-overrun", b, STATUS_DATA_ERROR);

    b = good;           // interface descriptor shorter than 9 bytes
    b[9] = 8;                              // first interface descriptor
    Run("interface-too-short", b, STATUS_DATA_ERROR);

    Block noAc;
    ConfigStart(noAc);
    Interface(noAc, 0, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x01, 0x00);
    InputTerminal(noAc, 0x03, 0x0301, 2);
    Interface(noAc, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(noAc, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(noAc, 0x03, 1);
    FormatTypeIDiscrete(noAc, 2, 3, 24, {48000});
    Endpoint(noAc, 0x01, 0x01, 192, 1);
    ConfigFinish(noAc, 2);
    Run("no-ac-header", noAc, STATUS_DEVICE_CONFIGURATION_ERROR);

    Block badSub;
    ConfigStart(badSub);
    Interface(badSub, 0, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x00, 0x00);
    AcHeader(badSub);
    Interface(badSub, 1, 0, 1, EMU_INTERFACE_CLASS_VENDOR, 0x00, 0x00);
    AsGeneral(badSub, 0x03, 1);
    FormatTypeIDiscrete(badSub, 2, 3, 24, {48000});
    Endpoint(badSub, 0x01, 0x01, 192, 1);
    ConfigFinish(badSub, 2);
    Run("subclass-hidden-too", badSub, STATUS_DEVICE_CONFIGURATION_ERROR);

    Block dangling;
    ConfigStart(dangling);
    AcInterface(dangling, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(dangling, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(dangling, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(dangling, 0x77, 1);        // no such terminal
    FormatTypeIDiscrete(dangling, 2, 3, 24, {48000});
    Endpoint(dangling, 0x01, 0x01, 192, 1);
    ConfigFinish(dangling, 2);
    Run("terminal-link-unknown", dangling, STATUS_DEVICE_CONFIGURATION_ERROR);

    Block conflict;
    ConfigStart(conflict);
    AcInterface(conflict, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(conflict, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(conflict, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(conflict, 0x03, 1);        // render terminal...
    FormatTypeIDiscrete(conflict, 2, 3, 24, {48000});
    Endpoint(conflict, 0x81, 0x01, 192, 1);  // ...but an IN data endpoint
    ConfigFinish(conflict, 2);
    Run("terminal-ep-direction-conflict", conflict, STATUS_DEVICE_CONFIGURATION_ERROR);

    Block twoData;
    ConfigStart(twoData);
    AcInterface(twoData, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(twoData, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(twoData, 1, 1, 2, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(twoData, 0x03, 1);
    FormatTypeIDiscrete(twoData, 2, 3, 24, {48000});
    Endpoint(twoData, 0x01, 0x01, 192, 1);
    Endpoint(twoData, 0x02, 0x01, 192, 1);   // second data endpoint
    ConfigFinish(twoData, 2);
    Run("two-data-endpoints", twoData, STATUS_DEVICE_CONFIGURATION_ERROR);

    Block twoFb;
    ConfigStart(twoFb);
    AcInterface(twoFb, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(twoFb, 1, 1, 3, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(twoFb, 0x03, 1);
    FormatTypeIDiscrete(twoFb, 2, 3, 24, {48000});
    Endpoint(twoFb, 0x01, 0x01, 192, 1);
    Endpoint(twoFb, 0x81, 0x11, 4, 1);         // one data + one feedback is legal
    Endpoint(twoFb, 0x82, 0x11, 4, 1);         // a second feedback is not
    ConfigFinish(twoFb, 2);
    Run("two-feedback-endpoints", twoFb, STATUS_DEVICE_CONFIGURATION_ERROR);

    Block fmtNoEp;
    ConfigStart(fmtNoEp);
    AcInterface(fmtNoEp, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(fmtNoEp, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(fmtNoEp, 1, 1, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(fmtNoEp, 0x03, 1);
    FormatTypeIDiscrete(fmtNoEp, 2, 3, 24, {48000});   // claims zero endpoints
    ConfigFinish(fmtNoEp, 2);
    Run("format-without-endpoint", fmtNoEp, STATUS_DEVICE_CONFIGURATION_ERROR);

    Block dupAlt;
    ConfigStart(dupAlt);
    AcInterface(dupAlt, 0, EMU_INTERFACE_CLASS_VENDOR);
    for (int i = 0; i < 2; i++) {
        Interface(dupAlt, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
        AsGeneral(dupAlt, 0x03, 1);
        FormatTypeIDiscrete(dupAlt, 2, 3, 24, {48000});
        Endpoint(dupAlt, 0x01, 0x01, 192, 1);
    }
    ConfigFinish(dupAlt, 2);
    Run("duplicate-alternate-setting", dupAlt, STATUS_DEVICE_CONFIGURATION_ERROR);

    Block badFeature;
    ConfigStart(badFeature);
    Interface(badFeature, 0, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x01, 0x00);
    AcHeader(badFeature);
    InputTerminal(badFeature, 0x03, 0x0301, 2);
    Bytes(badFeature, {10, 0x24, 0x06, 0x02, 0x01, 0x04});   // bControlSize 4, 3 bytes of controls
    Bytes(badFeature, {0x03, 0x03, 0x03, 0x00});
    Interface(badFeature, 1, 0, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(badFeature, 0x03, 1);
    FormatTypeIDiscrete(badFeature, 2, 3, 24, {48000});
    Endpoint(badFeature, 0x01, 0x01, 192, 1);
    ConfigFinish(badFeature, 2);
    Run("feature-unit-controls-undivided", badFeature, STATUS_DATA_ERROR);

    Block dupXu;
    ConfigStart(dupXu);
    Interface(dupXu, 0, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x01, 0x00);
    AcHeader(dupXu);
    InputTerminal(dupXu, 0x03, 0x0301, 2);
    XuEmu(dupXu, 0x0A, EMU_XU_CLOCK_RATE);
    XuEmu(dupXu, 0x0A, EMU_XU_CLOCK_SOURCE);      // same unit ID twice
    Interface(dupXu, 1, 0, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(dupXu, 0x03, 1);
    FormatTypeIDiscrete(dupXu, 2, 3, 24, {48000});
    Endpoint(dupXu, 0x01, 0x01, 192, 1);
    ConfigFinish(dupXu, 2);
    Run("duplicate-xu-unit-id", dupXu, STATUS_DEVICE_CONFIGURATION_ERROR);
}

// ---------------------------------------------------------------------------
// array bounds: overflow must fail, never truncate
// ---------------------------------------------------------------------------

static void CaseOverflowLimits(void)
{
    Block alts;
    ConfigStart(alts);
    AcInterface(alts, 0, EMU_INTERFACE_CLASS_VENDOR);
    for (unsigned i = 0; i <= EMU_MAX_STREAM_ALTS; i++) {   // one past the table
        Interface(alts, 1, i + 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
        AsGeneral(alts, 0x03, 1);
        FormatTypeIDiscrete(alts, 2, 3, 24, {48000 + i});
        Endpoint(alts, 0x01, 0x01, 192, 1);
    }
    ConfigFinish(alts, 2);
    Run("too-many-alternates", alts, STATUS_BUFFER_OVERFLOW);

    Block rates;
    ConfigStart(rates);
    AcInterface(rates, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(rates, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(rates, 0x03, 1);
    std::vector<unsigned> many;
    for (unsigned i = 0; i <= EMU_MAX_RATES_PER_ALT; i++) {
        many.push_back(8000 + i);
    }
    FormatTypeIDiscrete(rates, 2, 3, 24, many);
    Endpoint(rates, 0x01, 0x01, 192, 1);
    ConfigFinish(rates, 2);
    Run("too-many-rates", rates, STATUS_BUFFER_OVERFLOW);

    Block xus;
    ConfigStart(xus);
    Interface(xus, 0, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x01, 0x00);
    AcHeader(xus);
    InputTerminal(xus, 0x03, 0x0301, 2);
    for (unsigned i = 0; i <= EMU_MAX_XU_UNITS; i++) {
        XuEmu(xus, 0x40 + i, EMU_XU_CLOCK_RATE + (i % 6));
    }
    Interface(xus, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(xus, 0x03, 1);
    FormatTypeIDiscrete(xus, 2, 3, 24, {48000});
    Endpoint(xus, 0x01, 0x01, 192, 1);
    ConfigFinish(xus, 2);
    Run("too-many-xu-units", xus, STATUS_BUFFER_OVERFLOW);

    Block ifaces;
    ConfigStart(ifaces);
    for (unsigned i = 0; i <= EMU_MAX_AUDIO_INTERFACES; i++) {
        Interface(ifaces, i, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x00, 0x00);
    }
    ConfigFinish(ifaces, EMU_MAX_AUDIO_INTERFACES + 1);
    Run("too-many-interfaces", ifaces, STATUS_BUFFER_OVERFLOW);

    // Exactly at the limit must still parse: the boundary is inclusive.
    Block fitAlts;
    ConfigStart(fitAlts);
    AcInterface(fitAlts, 0, EMU_INTERFACE_CLASS_VENDOR);
    for (unsigned i = 0; i < EMU_MAX_STREAM_ALTS; i++) {
        Interface(fitAlts, 1, i + 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
        AsGeneral(fitAlts, 0x03, 1);
        FormatTypeIDiscrete(fitAlts, 2, 3, 24, {48000});
        Endpoint(fitAlts, 0x01, 0x01, 192, 1);
    }
    ConfigFinish(fitAlts, 2);
    static EMU_DEVICE_CAPS caps;
    g_case = "exactly-at-limit";
    CheckStatus("exactly-at-limit", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(fitAlts.data(), (ULONG)fitAlts.size(), &caps));
    CHECK_EQ(caps.StreamAltCount, EMU_MAX_STREAM_ALTS);
}

// ---------------------------------------------------------------------------
// interface descriptor lookup (the USB selection requests need byte
// offsets into the configuration block)
// ---------------------------------------------------------------------------

static void CaseInterfaceDescriptorLookup(void)
{
    g_case = "iface-desc-lookup";
    static EMU_DEVICE_CAPS caps;
    CHECK(Parse0404(&caps));

    // Every interface of the live device is findable at its zero-bandwidth
    // setting, and so are the streaming alternates M3 will select.
    CHECK_GE((long)EmuCapsFindInterfaceDescriptor(
             Emu0404RealDescriptor, sizeof(Emu0404RealDescriptor), 0, 0), 0);
    CHECK_GE((long)EmuCapsFindInterfaceDescriptor(
             Emu0404RealDescriptor, sizeof(Emu0404RealDescriptor), 1, 0), 0);
    CHECK_GE((long)EmuCapsFindInterfaceDescriptor(
             Emu0404RealDescriptor, sizeof(Emu0404RealDescriptor), 2, 0), 0);
    CHECK_GE((long)EmuCapsFindInterfaceDescriptor(
             Emu0404RealDescriptor, sizeof(Emu0404RealDescriptor), 3, 0), 0);
    LONG r48 = EmuCapsFindInterfaceDescriptor(
        Emu0404RealDescriptor, sizeof(Emu0404RealDescriptor), 1, 3);
    CHECK_GE(r48, 0);
    // The found block really is the requested interface descriptor.
    CHECK_EQ(Emu0404RealDescriptor[r48], 9);
    CHECK_EQ(Emu0404RealDescriptor[r48 + 1], 0x04);
    CHECK_EQ(Emu0404RealDescriptor[r48 + 2], 1);
    CHECK_EQ(Emu0404RealDescriptor[r48 + 3], 3);
    CHECK_GE((long)EmuCapsFindInterfaceDescriptor(
             Emu0404RealDescriptor, sizeof(Emu0404RealDescriptor), 2, 16), 0);
    CHECK_GE((long)EmuCapsFindInterfaceDescriptor(
             Emu0404RealDescriptor, sizeof(Emu0404RealDescriptor), 1, 18), 0);

    // Missing combinations read as -1, never as a wrong offset.
    CHECK_EQ(EmuCapsFindInterfaceDescriptor(
             Emu0404RealDescriptor, sizeof(Emu0404RealDescriptor), 4, 0), -1);
    CHECK_EQ(EmuCapsFindInterfaceDescriptor(
             Emu0404RealDescriptor, sizeof(Emu0404RealDescriptor), 1, 19), -1);
    CHECK_EQ(EmuCapsFindInterfaceDescriptor(
             Emu0404RealDescriptor, sizeof(Emu0404RealDescriptor), 0, 1), -1);

    // A block that ends mid-walk cannot point anywhere.
    UCHAR tail[10] = {9, 0x04, 1, 0, 1, 0xFF, 0x02, 0x00, 0x00, 5};
    CHECK_GE((long)EmuCapsFindInterfaceDescriptor(tail, 9, 1, 0), 0);
    CHECK_EQ(EmuCapsFindInterfaceDescriptor(tail, 1, 1, 0), -1);
}

// ---------------------------------------------------------------------------
// rate and alternate-setting lookup
// ---------------------------------------------------------------------------

static void CaseAltLookup(void)
{
    g_case = "alt-lookup";
    static EMU_DEVICE_CAPS caps;
    CHECK(Parse0404(&caps));

    // Render alt table order: alts 1-10 are 2ch/24-bit (each rate doubled into
    // a high- and a low-bandwidth alt), alts 11-16 are 4ch, alts 17-18 are
    // 2ch/16-bit. The lookup must answer with the first alt in descriptor
    // order, which is what makes the choice reproducible.
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 2, 44100),
             &caps.StreamAlts[0]);
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 2, 48000),
             &caps.StreamAlts[2]);
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 2, 96000),
             &caps.StreamAlts[6]);
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 2, 176400),
             &caps.StreamAlts[8]);
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 2, 192000),
             &caps.StreamAlts[9]);
    // 4ch render stops at 96 kHz, and the lookup must not fall back onto the
    // 2ch alt: a caller that asked for four channels and got two would program
    // a frame the device does not have.
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 4, 44100),
             &caps.StreamAlts[10]);
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 4, 96000),
             &caps.StreamAlts[15]);
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 4, 176400), NULL);
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 4, 192000), NULL);
    // Nothing in the descriptor is 1-channel or 3-channel.
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 1, 48000), NULL);
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 8, 48000), NULL);

    // Capture mirrors the render table (16 alts, two bandwidth settings per
    // rate) and reaches 192 kHz as 2ch — the M5 claim "capture tops out lower"
    // is a fixture myth the real descriptor kills.
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_CAPTURE, 2, 44100),
             &caps.StreamAlts[16]);
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_CAPTURE, 2, 48000),
             &caps.StreamAlts[18]);
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_CAPTURE, 2, 192000),
             &caps.StreamAlts[25]);
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_CAPTURE, 4, 192000), NULL);

    // Device-wide: 192000 is reachable through 2ch even though no 4ch alt has
    // it, which is exactly why the claim is deliberately weak.
    CHECK(EmuCapsSupportsRate(&caps, 44100));
    CHECK(EmuCapsSupportsRate(&caps, 48000));
    CHECK(EmuCapsSupportsRate(&caps, 176400));
    CHECK(EmuCapsSupportsRate(&caps, 192000));
    CHECK(!EmuCapsSupportsRate(&caps, 88201));
    CHECK(!EmuCapsSupportsRate(&caps, 8000));
    CHECK(!EmuCapsSupportsRate(&caps, 192001));
    // A rate of zero is what a failed GET decodes to; it must never look valid.
    CHECK(!EmuCapsSupportsRate(&caps, 0));
    CHECK_EQ(EmuCapsFindAlt(&caps, EMU_STREAM_RENDER, 2, 0), NULL);

    // Continuous range: both bounds inclusive.
    Block b;
    ConfigStart(b);
    AcInterface(b, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(b, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIContinuous(b, 2, 3, 24, 8000, 192000);
    Endpoint(b, 0x01, 0x01, 192, 1);
    ConfigFinish(b, 2);
    static EMU_DEVICE_CAPS cont;
    CheckStatus("continuous-lookup", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &cont));
    CHECK(EmuCapsSupportsRate(&cont, 8000));
    CHECK(EmuCapsSupportsRate(&cont, 44100));
    CHECK(EmuCapsSupportsRate(&cont, 192000));
    CHECK(!EmuCapsSupportsRate(&cont, 7999));
    CHECK(!EmuCapsSupportsRate(&cont, 192001));
    CHECK_EQ(EmuCapsFindAlt(&cont, EMU_STREAM_RENDER, 2, 176400),
             &cont.StreamAlts[0]);

    // Discrete and continuous alt for the same channels and rate: the answer is
    // the first one in descriptor order, so the choice is reproducible rather
    // than dependent on how the table happened to fill in.
    Block both;
    ConfigStart(both);
    AcInterface(both, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(both, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(both, 0x03, 1);
    FormatTypeIContinuous(both, 2, 3, 24, 8000, 192000);
    Endpoint(both, 0x01, 0x01, 192, 1);
    Interface(both, 1, 2, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(both, 0x03, 1);
    FormatTypeIDiscrete(both, 2, 3, 24, {48000});
    Endpoint(both, 0x02, 0x01, 192, 1);
    ConfigFinish(both, 2);
    static EMU_DEVICE_CAPS mixed;
    CheckStatus("discrete-and-continuous", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(both.data(), (ULONG)both.size(), &mixed));
    CHECK_EQ(EmuCapsFindAlt(&mixed, EMU_STREAM_RENDER, 2, 48000),
             &mixed.StreamAlts[0]);
    CHECK_EQ(mixed.StreamAlts[0].AltSetting, 1);
    // The alt picked above has to be the one whose endpoints the caller will
    // program: the continuous alt is the one with endpoint 0x01.
    CHECK_EQ(mixed.StreamAlts[0].DataEndpoint, 0x01);
    CHECK_EQ(EmuCapsFindAlt(&mixed, EMU_STREAM_RENDER, 2, 96000),
             &mixed.StreamAlts[0]);
    // A rate no discrete list names is still reached, through the continuous
    // range; one past both bounds is not supported at all.
    CHECK(EmuCapsSupportsRate(&mixed, 44101));
    CHECK(!EmuCapsSupportsRate(&mixed, 192001));

    // An empty table answers "no" to everything instead of reading past it.
    static EMU_DEVICE_CAPS empty;
    EmuParseUsbConfigDescriptor(b.data(), 0, &empty);
    CHECK_EQ(empty.StreamAltCount, 0u);
    CHECK(!EmuCapsSupportsRate(&empty, 48000));
    CHECK_EQ(EmuCapsFindAlt(&empty, EMU_STREAM_RENDER, 2, 48000), NULL);
    CHECK_EQ(EmuCapsAltCount(&empty, EMU_STREAM_RENDER), 0u);
}

static void CaseRateList(void)
{
    g_case = "rate-list";
    static EMU_DEVICE_CAPS caps;
    ULONG rates[EMU_MAX_REPORTED_RATES];
    ULONG flags;
    ULONG count;

    // The 0404 fixture repeats 44.1/48 kHz across three alternate settings and
    // 88.2/96 across two: the list is the distinct union, ascending, and the
    // repeats are what makes deduplication worth testing.
    CHECK(Parse0404(&caps));
    count = EmuCapsRateList(&caps, rates, EMU_MAX_REPORTED_RATES, &flags);
    CHECK_EQ(count, 6u);
    CHECK_EQ(flags, 0u);
    CHECK_EQ(rates[0], 44100u);
    CHECK_EQ(rates[1], 48000u);
    CHECK_EQ(rates[2], 88200u);
    CHECK_EQ(rates[3], 96000u);
    CHECK_EQ(rates[4], 176400u);
    CHECK_EQ(rates[5], 192000u);

    // A smaller report is not an error: the lowest rates come back and the flag
    // says the truth was longer.
    count = EmuCapsRateList(&caps, rates, 3, &flags);
    CHECK_EQ(count, 3u);
    CHECK_EQ(flags, EMU_RATES_TRUNCATED);
    CHECK_EQ(rates[0], 44100u);
    CHECK_EQ(rates[2], 88200u);
    count = EmuCapsRateList(&caps, rates, 6, &flags);
    CHECK_EQ(count, 6u);
    CHECK_EQ(flags, 0u);                     // exactly enough room is not truncation
    count = EmuCapsRateList(&caps, rates, 0, &flags);
    CHECK_EQ(count, 0u);
    CHECK_EQ(flags, EMU_RATES_TRUNCATED);

    // A range has no enumerable membership, so nothing is listed and the flag is
    // the only answer a continuous device can give.
    Block b;
    ConfigStart(b);
    AcInterface(b, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(b, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIContinuous(b, 2, 3, 24, 8000, 192000);
    Endpoint(b, 0x01, 0x01, 192, 1);
    ConfigFinish(b, 2);
    static EMU_DEVICE_CAPS cont;
    CheckStatus("rate-list-continuous", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &cont));
    count = EmuCapsRateList(&cont, rates, EMU_MAX_REPORTED_RATES, &flags);
    CHECK_EQ(count, 0u);
    CHECK_EQ(flags, EMU_RATES_CONTINUOUS);

    // Both kinds on one device: the discrete entries still come, and the range
    // makes them an incomplete picture, which is what the flag reports.
    Interface(b, 1, 2, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIDiscrete(b, 2, 3, 24, {48000, 44100});
    Endpoint(b, 0x02, 0x01, 192, 1);
    ConfigFinish(b, 2);
    static EMU_DEVICE_CAPS mixed;
    CheckStatus("rate-list-mixed", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &mixed));
    count = EmuCapsRateList(&mixed, rates, EMU_MAX_REPORTED_RATES, &flags);
    CHECK_EQ(count, 2u);                     // the range contributes nothing to list
    CHECK_EQ(flags, EMU_RATES_CONTINUOUS);
    CHECK_EQ(rates[0], 44100u);              // unsorted input, sorted output
    CHECK_EQ(rates[1], 48000u);

    // Nothing recorded, nothing to say.
    static EMU_DEVICE_CAPS empty;
    EmuParseUsbConfigDescriptor(b.data(), 0, &empty);
    flags = 0xFFFF;
    count = EmuCapsRateList(&empty, rates, EMU_MAX_REPORTED_RATES, &flags);
    CHECK_EQ(count, 0u);
    CHECK_EQ(flags, 0u);
}

static void CaseApiGuards(void)
{
    g_case = "api-guards";
    static EMU_DEVICE_CAPS caps;
    Block b;
    ConfigStart(b);
    AcInterface(b, 0, EMU_INTERFACE_CLASS_VENDOR);
    Interface(b, 1, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIDiscrete(b, 2, 3, 24, {48000});
    Endpoint(b, 0x01, 0x01, 192, 1);
    ConfigFinish(b, 2);

    CheckStatus("null descriptor", STATUS_INVALID_PARAMETER,
                EmuParseUsbConfigDescriptor(NULL, (ULONG)b.size(), &caps));
    CheckStatus("null caps", STATUS_INVALID_PARAMETER,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), NULL));
    unsigned char tiny[8] = {9, 0x02, 8, 0, 1, 1, 0, 0xC0};
    CheckStatus("buffer below header", STATUS_DATA_ERROR,
                EmuParseUsbConfigDescriptor(tiny, sizeof(tiny), &caps));

    // Caps must be fully rewritten on a failed parse, not left stale: the
    // caller keeps one structure across start/stop cycles.
    CheckStatus("first-parse", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &caps));
    CHECK_EQ(caps.StreamAltCount, 1);
    b[b.size() - 7] = 0;
    CheckStatus("second-parse-fails", STATUS_DATA_ERROR,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &caps));
    CHECK_EQ(caps.StreamAltCount, 0);
    CHECK_EQ(caps.ParsedFlags, 0u);
    CHECK_EQ(caps.AcInterfaceNumber, EMU_NO_VALUE);
}

int
main(void)
{
    printf("openEMUdriver EmuUSBAudio descriptor parser test\n");
    printf("(parser: driver/emuusbaudio/Emu/emu_desc.cpp, same source as in the driver)\n");

    CaseBaseline0404();
    CaseStandardClassDeclaration();
    CaseXuBothLayouts();
    CaseXuMalformed();
    CaseContinuousRates();
    CaseNonPcmAltDropped();
    CaseEndpointOrderReversed();
    CaseMalformed();
    CaseOverflowLimits();
    CaseAltLookup();
    CaseInterfaceDescriptorLookup();
    CaseRateList();
    CaseApiGuards();

    return TestResult();
}
