// EmuUSBAudio IOCTL-contract unit test, user-mode.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Build and run:  cd driver && ./utest.sh ioctl
//
// What is under test is the part of the panel contract that a device cannot
// correct: which requests the driver answers at all, and what the bytes it
// answers mean. The replies are filled from a descriptor the parser really
// produced (tests/descfixture.h), so a mask that only looks right because a
// struct was hand-filled cannot pass here.
#include <stddef.h>          // offsetof, for the wire-layout case
#include "descfixture.h"
#include "emu_ioctl.h"
#include "testharness.h"

int g_checks = 0;
int g_failures = 0;
const char* g_case = "";

// EMU_CAPS_INFO is 40 ULONGs and EMU_STATE_INFO 13, with no pointer and no
// 64-bit scalar anywhere: if a field type ever changes, these stop holding and
// the panel would be reading a different structure than the driver wrote.
C_ASSERT(sizeof(EMU_CAPS_INFO) == 40 * sizeof(ULONG));
C_ASSERT(sizeof(EMU_STATE_INFO) == 13 * sizeof(ULONG));

static void Decide(_Out_ PEMU_IOCTL_DECISION d, _In_ ULONG code,
                   _In_ BOOLEAN usable, _In_ ULONG inLen, _In_ ULONG outLen)
{
    EMU_IOCTL_CALL call;
    call.ControlCode = code;
    call.InputBufferLength = inLen;
    call.OutputBufferLength = outLen;
    EmuIoctlClassify(&call, usable, d);
}

// Anything that is not exactly one of the two defined codes goes back to
// PortCls, including neighbours in the same device type and method. Forwarding
// is what keeps KS/wdmaud working against this FDO, so a "helpful" refusal of
// unknown codes would be a bug report about Windows audio, not about the panel.
static void CaseForward(void)
{
    g_case = "forward";
    EMU_IOCTL_DECISION d;

    // KS's own device type (0x2f), the traffic this driver must not swallow.
    Decide(&d, CTL_CODE(FILE_DEVICE_KS, 0x000, METHOD_NEITHER, FILE_ANY_ACCESS),
           TRUE, 0, 0);
    CHECK_EQ(d.Action, EmuIoctlForward);
    CheckStatus("ks-property", STATUS_SUCCESS, d.Status);
    CHECK_EQ(d.OutputBytes, 0u);

    // The next function code in our own range: an operation a newer panel
    // invents is not this build's to interpret.
    Decide(&d, CTL_CODE(EMU_IOCTL_BASE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS),
           TRUE, 0, 256);
    CHECK_EQ(d.Action, EmuIoctlForward);

    // Same function code, different method or access class: a different number,
    // so a different request, and not one this driver defined.
    Decide(&d, CTL_CODE(EMU_IOCTL_BASE, 0x800, METHOD_NEITHER, FILE_READ_ACCESS),
           TRUE, 0, 256);
    CHECK_EQ(d.Action, EmuIoctlForward);
    Decide(&d, CTL_CODE(EMU_IOCTL_BASE, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS),
           TRUE, 0, 256);
    CHECK_EQ(d.Action, EmuIoctlForward);
    Decide(&d, CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS),
           TRUE, 0, 256);
    CHECK_EQ(d.Action, EmuIoctlGetCaps);        // 0x800 on 0x22 is ours, spelled either way
    Decide(&d, 0, TRUE, 0, 0);
    CHECK_EQ(d.Action, EmuIoctlForward);

    // Not ready does not change a forward: PortCls still has to see the IRP.
    Decide(&d, CTL_CODE(FILE_DEVICE_KS, 0x001, METHOD_BUFFERED, FILE_ANY_ACCESS),
           FALSE, 0, 0);
    CHECK_EQ(d.Action, EmuIoctlForward);
    CheckStatus("forward-while-not-ready", STATUS_SUCCESS, d.Status);
}

// One of ours, but the instance cannot currently be talked to. The device answer
// comes before the buffer answers: "not ready" is the useful reason during a
// stop or a surprise removal, whatever the caller's buffers looked like.
static void CaseNotReady(void)
{
    g_case = "not-ready";
    EMU_IOCTL_DECISION d;

    Decide(&d, EMU_IOCTL_GET_CAPS, FALSE, 0, sizeof(EMU_CAPS_INFO));
    CHECK_EQ(d.Action, EmuIoctlReject);
    CheckStatus("caps-not-ready", STATUS_DEVICE_NOT_READY, d.Status);
    CHECK_EQ(d.OutputBytes, 0u);

    Decide(&d, EMU_IOCTL_GET_STATE, FALSE, 0, sizeof(EMU_STATE_INFO));
    CHECK_EQ(d.Action, EmuIoctlReject);
    CheckStatus("state-not-ready", STATUS_DEVICE_NOT_READY, d.Status);

    // A request that is badly formed *and* aimed at a stopped device is still
    // refused for the device: the caller learns the thing it can act on.
    Decide(&d, EMU_IOCTL_GET_CAPS, FALSE, 4, 0);
    CheckStatus("not-ready-beats-buffer", STATUS_DEVICE_NOT_READY, d.Status);
}

// The METHOD_BUFFERED rules of the two queries.
static void CaseBufferRules(void)
{
    g_case = "buffers";
    EMU_IOCTL_DECISION d;

    Decide(&d, EMU_IOCTL_GET_CAPS, TRUE, 0, sizeof(EMU_CAPS_INFO));
    CHECK_EQ(d.Action, EmuIoctlGetCaps);
    CheckStatus("caps-exact", STATUS_SUCCESS, d.Status);
    CHECK_EQ(d.OutputBytes, sizeof(EMU_CAPS_INFO));

    Decide(&d, EMU_IOCTL_GET_STATE, TRUE, 0, sizeof(EMU_STATE_INFO));
    CHECK_EQ(d.Action, EmuIoctlGetState);
    CHECK_EQ(d.OutputBytes, sizeof(EMU_STATE_INFO));

    // A bigger buffer is accepted and read only as far as this build writes,
    // which is what makes appending fields a compatible change.
    Decide(&d, EMU_IOCTL_GET_CAPS, TRUE, 0, sizeof(EMU_CAPS_INFO) + 4096);
    CHECK_EQ(d.Action, EmuIoctlGetCaps);
    CHECK_EQ(d.OutputBytes, sizeof(EMU_CAPS_INFO));

    Decide(&d, EMU_IOCTL_GET_CAPS, TRUE, 0, sizeof(EMU_CAPS_INFO) - 1);
    CHECK_EQ(d.Action, EmuIoctlReject);
    CheckStatus("caps-short", STATUS_BUFFER_TOO_SMALL, d.Status);
    CHECK_EQ(d.OutputBytes, 0u);

    // The state query's input rule doubles as the boundary between queries and
    // writes: GET_STATE with an input stage is refused (it would mean a
    // dialect this build does not speak), while the SET codes REQUIRE it.
    Decide(&d, EMU_IOCTL_GET_STATE, TRUE, 4, sizeof(EMU_STATE_INFO));
    CheckStatus("state-with-input", STATUS_INVALID_PARAMETER, d.Status);
}

// The four SET operations: fixed EMU_SET_INPUT stage in, refreshed
// EMU_STATE_INFO out. The access bits make a SET a different control code
// number from the query on the same function code, and the reply size is the
// state structure for every one of them - the panel sees the accepted value
// and LastOperationStatus in one roundtrip.
static void CaseSetRules(void)
{
    g_case = "set-rules";
    EMU_IOCTL_DECISION d;
    const ULONG codes[] = {EMU_IOCTL_SET_RATE, EMU_IOCTL_SET_CLOCK_SOURCE,
                           EMU_IOCTL_SET_SPDIF_FORMAT, EMU_IOCTL_SET_SOFT_LIMIT};
    const EMU_IOCTL_ACTION actions[] = {EmuIoctlSetRate, EmuIoctlSetClockSource,
                                        EmuIoctlSetSpdifFormat,
                                        EmuIoctlSetSoftLimit};

    for (ULONG i = 0; i < 4; i++) {
        Decide(&d, codes[i], TRUE, sizeof(EMU_SET_INPUT),
               sizeof(EMU_STATE_INFO));
        CHECK_EQ(d.Action, actions[i]);
        CheckStatus("set-exact", STATUS_SUCCESS, d.Status);
        CHECK_EQ(d.OutputBytes, sizeof(EMU_STATE_INFO));

        // A bigger stage is read only as far as this build's fields; a bigger
        // output is the usual append-only compatibility.
        Decide(&d, codes[i], TRUE, sizeof(EMU_SET_INPUT) + 64,
               sizeof(EMU_STATE_INFO) + 4096);
        CHECK_EQ(d.Action, actions[i]);
        CHECK_EQ(d.OutputBytes, sizeof(EMU_STATE_INFO));

        // A short input stage cannot carry Size/Version - refused before the
        // device is touched.
        Decide(&d, codes[i], TRUE, sizeof(EMU_SET_INPUT) - 1,
               sizeof(EMU_STATE_INFO));
        CheckStatus("set-short-input", STATUS_INVALID_PARAMETER, d.Status);

        // No output stage means the verdict (LastOperationStatus) would not
        // reach the caller - refused too.
        Decide(&d, codes[i], TRUE, sizeof(EMU_SET_INPUT), 0);
        CheckStatus("set-no-output", STATUS_BUFFER_TOO_SMALL, d.Status);

        // Not ready beats everything else, exactly like the queries.
        Decide(&d, codes[i], FALSE, sizeof(EMU_SET_INPUT),
               sizeof(EMU_STATE_INFO));
        CHECK_EQ(d.Action, EmuIoctlReject);
        CheckStatus("set-not-ready", STATUS_DEVICE_NOT_READY, d.Status);
    }

    // Same function code with query access bits is a different control code -
    // the SET is not a query with an input stage, and the query is not a SET.
    Decide(&d, CTL_CODE(EMU_IOCTL_BASE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS),
           TRUE, sizeof(EMU_SET_INPUT), sizeof(EMU_STATE_INFO));
    CHECK_EQ(d.Action, EmuIoctlForward);

    Decide(&d, EMU_IOCTL_GET_STATE, TRUE, 0, sizeof(EMU_STATE_INFO) - 4);
    CheckStatus("state-short", STATUS_BUFFER_TOO_SMALL, d.Status);
    Decide(&d, EMU_IOCTL_GET_CAPS, TRUE, 0, 0);
    CheckStatus("no-output", STATUS_BUFFER_TOO_SMALL, d.Status);

    // Both are queries: an input stage means the caller is sending bytes this
    // contract has no place for, and guessing is how a panel retunes a device.
    Decide(&d, EMU_IOCTL_GET_CAPS, TRUE, 1, sizeof(EMU_CAPS_INFO));
    CHECK_EQ(d.Action, EmuIoctlReject);
    CheckStatus("caps-input", STATUS_INVALID_PARAMETER, d.Status);
    Decide(&d, EMU_IOCTL_GET_STATE, TRUE, sizeof(EMU_STATE_INFO),
           sizeof(EMU_STATE_INFO));
    CheckStatus("state-input", STATUS_INVALID_PARAMETER, d.Status);
}

// The M3 test-slice codes: START and STOP take the SET input stage and answer
// the test stats; STATS is a query. Not panel operations - no EMU_OP_ bit,
// no SupportedMask entry - but the buffer rules are contract-grade all the
// same, because emu-probe is only the first client of the layout.
static void CaseTestStreamRules(void)
{
    g_case = "test-stream";
    EMU_IOCTL_DECISION d;

    // START with the exact stage and the stats reply.
    Decide(&d, EMU_IOCTL_TEST_STREAM_START, TRUE, sizeof(EMU_SET_INPUT),
           sizeof(EMU_TEST_STREAM_STATS));
    CHECK_EQ(d.Action, EmuIoctlTestStart);
    CheckStatus("test-start-exact", STATUS_SUCCESS, d.Status);
    CHECK_EQ(d.OutputBytes, sizeof(EMU_TEST_STREAM_STATS));

    // STOP takes the same stage; its value is ignored by the contract.
    Decide(&d, EMU_IOCTL_TEST_STREAM_STOP, TRUE, sizeof(EMU_SET_INPUT),
           sizeof(EMU_TEST_STREAM_STATS));
    CHECK_EQ(d.Action, EmuIoctlTestStop);
    CHECK_EQ(d.OutputBytes, sizeof(EMU_TEST_STREAM_STATS));

    // A short input stage cannot carry Size/Version - refused before the
    // engine is touched, exactly like a SET.
    Decide(&d, EMU_IOCTL_TEST_STREAM_START, TRUE, sizeof(EMU_SET_INPUT) - 1,
           sizeof(EMU_TEST_STREAM_STATS));
    CheckStatus("test-start-short-input", STATUS_INVALID_PARAMETER, d.Status);

    // The stats reply is the only answer these codes give - without room for
    // it there is nothing to say.
    Decide(&d, EMU_IOCTL_TEST_STREAM_START, TRUE, sizeof(EMU_SET_INPUT),
           sizeof(EMU_TEST_STREAM_STATS) - 1);
    CheckStatus("test-start-short-output", STATUS_BUFFER_TOO_SMALL, d.Status);
    Decide(&d, EMU_IOCTL_TEST_STREAM_STOP, TRUE, sizeof(EMU_SET_INPUT), 0);
    CheckStatus("test-stop-no-output", STATUS_BUFFER_TOO_SMALL, d.Status);

    // STATS is a query: no input stage, stats-sized output, "not ready"
    // outranks the buffers like every other code.
    Decide(&d, EMU_IOCTL_TEST_STREAM_STATS, TRUE, 0,
           sizeof(EMU_TEST_STREAM_STATS));
    CHECK_EQ(d.Action, EmuIoctlTestStats);
    CheckStatus("test-stats-exact", STATUS_SUCCESS, d.Status);
    CHECK_EQ(d.OutputBytes, sizeof(EMU_TEST_STREAM_STATS));
    Decide(&d, EMU_IOCTL_TEST_STREAM_STATS, TRUE, 4,
           sizeof(EMU_TEST_STREAM_STATS));
    CheckStatus("test-stats-with-input", STATUS_INVALID_PARAMETER, d.Status);
    Decide(&d, EMU_IOCTL_TEST_STREAM_STATS, TRUE, 0,
           sizeof(EMU_TEST_STREAM_STATS) - 1);
    CheckStatus("test-stats-short-output", STATUS_BUFFER_TOO_SMALL, d.Status);
    Decide(&d, EMU_IOCTL_TEST_STREAM_STATS, FALSE, 0,
           sizeof(EMU_TEST_STREAM_STATS));
    CHECK_EQ(d.Action, EmuIoctlReject);
    CheckStatus("test-stats-not-ready", STATUS_DEVICE_NOT_READY, d.Status);

    // The neighbouring function code stays foreign: 0x809 forward, the way
    // 0x802 with query access bits was already pinned.
    Decide(&d, CTL_CODE(EMU_IOCTL_BASE, 0x809, METHOD_BUFFERED, FILE_READ_ACCESS),
           TRUE, 0, sizeof(EMU_TEST_STREAM_STATS));
    CHECK_EQ(d.Action, EmuIoctlForward);
}

static void CaseCapsFill0404(void)
{
    g_case = "caps-fill-0404";
    static EMU_DEVICE_CAPS caps;
    EMU_CAPS_INFO info;
    ULONG rates[EMU_MAX_REPORTED_RATES];
    ULONG flags;
    ULONG count;

    CHECK(Parse0404(&caps));
    info.Size = 0;                    // FillCaps must overwrite a stale header
    info.Version = 0;
    EmuIoctlFillCaps(&info, &caps, EMU_OP_IMPLEMENTED);

    CHECK_EQ(info.Size, sizeof(EMU_CAPS_INFO));
    CHECK_EQ(info.Version, EMU_IOCTL_VERSION);
    CHECK_EQ(info.ImplementedMask, EMU_OP_IMPLEMENTED);
    // The fixture carries all four controllable units, so every SET the contract
    // names is supportable - and none of them is implemented by this build, so
    // the two masks must disagree. The panel may only offer the intersection.
    CHECK_EQ(info.SupportedMask,
             EMU_OP_GET_CAPS | EMU_OP_GET_STATE | EMU_OP_SET_RATE |
             EMU_OP_SET_CLOCK_SOURCE | EMU_OP_SET_SPDIF_FORMAT |
             EMU_OP_SET_SOFT_LIMIT);
    CHECK(!(info.SupportedMask & EMU_OP_GET_STREAM_STATS));
    CHECK_EQ((info.SupportedMask & info.ImplementedMask), EMU_OP_IMPLEMENTED);

    // Straight from the parsed descriptor, not recomputed.
    CHECK_EQ(info.ParsedFlags, caps.ParsedFlags);
    CHECK_EQ(info.ConfigurationValue, 1u);
    CHECK_EQ(info.AcInterfaceNumber, 0u);
    CHECK_EQ(info.RenderAltCount, 16u);
    CHECK_EQ(info.CaptureAltCount, 16u);
    CHECK_EQ(info.MaxChannels, 4u);
    CHECK_EQ(info.MinRateHz, 44100u);
    CHECK_EQ(info.MaxRateHz, 192000u);
    CHECK_EQ(info.StatusEndpoint, 0x83u);
    CHECK_EQ(info.FeatureUnitCount, 1u);
    CHECK_EQ(info.FeatureUnitId, 0x04u);
    CHECK_EQ(info.FeatureMasterControls, 0x01u);   // master is mute-only on this firmware
    CHECK_EQ(info.ClockRateUnitId, 0x0Cu);
    CHECK_EQ(info.ClockSourceUnitId, 0x0Du);
    CHECK_EQ(info.DigitalIoUnitId, 0x0Eu);
    CHECK_EQ(info.DeviceOptionsUnitId, 0x0Fu);
    // The captured firmware carries only codes 0xE301..0xE304: the panel sees
    // "no such unit" for direct monitor and metering instead of a unit id it
    // cannot address.
    CHECK_EQ(info.DirectMonitorUnitId, EMU_NO_VALUE);
    CHECK_EQ(info.MeteringUnitId, EMU_NO_VALUE);

    // The reply's list is exactly what the parser's own query returns, in the
    // same order: one implementation, so the two cannot drift apart.
    count = EmuCapsRateList(&caps, rates, EMU_MAX_REPORTED_RATES, &flags);
    CHECK_EQ(info.RateCount, count);
    CHECK_EQ(info.RateCount, 6u);
    CHECK_EQ(info.RateFlags, flags);
    CHECK_EQ(info.RateFlags, 0u);            // every alt in the fixture is discrete
    for (ULONG i = 0; i < count; i++) {
        CHECK_EQ(info.Rates[i], rates[i]);
    }
    CHECK_EQ(info.Rates[0], 44100u);
    CHECK_EQ(info.Rates[1], 48000u);
    CHECK_EQ(info.Rates[2], 88200u);
    CHECK_EQ(info.Rates[3], 96000u);
    CHECK_EQ(info.Rates[4], 176400u);
    CHECK_EQ(info.Rates[5], 192000u);
    // Nothing is left unassigned past the list: the whole tail of the array is
    // part of the buffer the caller receives.
    for (ULONG i = count; i < EMU_MAX_REPORTED_RATES; i++) {
        CHECK_EQ(info.Rates[i], 0u);
    }
}

// A device that has none of the E-MU units and advertises a continuous range:
// the two cases where a mask or a rate list could claim something the descriptor
// does not carry.
static void CaseCapsFillSparse(void)
{
    g_case = "caps-fill-sparse";
    Block b;
    static EMU_DEVICE_CAPS caps;
    EMU_CAPS_INFO info;

    ConfigStart(b);
    Interface(b, 0, 0, 0, EMU_INTERFACE_CLASS_AUDIO, 0x01, 0x00);
    AcHeader(b);
    InputTerminal(b, 0x03, 0x0301, 2);
    Interface(b, 1, 1, 1, EMU_INTERFACE_CLASS_AUDIO, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIContinuous(b, 2, 3, 24, 8000, 192000);
    Endpoint(b, 0x01, 0x01, 192, 1);
    ConfigFinish(b, 2);
    CheckStatus("sparse-parse", STATUS_SUCCESS,
                EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), &caps));

    EmuIoctlFillCaps(&info, &caps, EMU_OP_IMPLEMENTED);
    CHECK_EQ(info.SupportedMask, EMU_OP_GET_CAPS | EMU_OP_GET_STATE);
    CHECK(!(info.ParsedFlags & EMU_CAPS_HAS_XU));
    CHECK_EQ(info.ClockRateUnitId, EMU_NO_VALUE);
    CHECK_EQ(info.ClockSourceUnitId, EMU_NO_VALUE);
    CHECK_EQ(info.DigitalIoUnitId, EMU_NO_VALUE);
    CHECK_EQ(info.DeviceOptionsUnitId, EMU_NO_VALUE);
    CHECK_EQ(info.DirectMonitorUnitId, EMU_NO_VALUE);
    CHECK_EQ(info.MeteringUnitId, EMU_NO_VALUE);
    CHECK_EQ(info.FeatureUnitCount, 0u);
    CHECK_EQ(info.FeatureUnitId, EMU_NO_VALUE);
    CHECK_EQ(info.FeatureMasterControls, EMU_NO_VALUE);
    // Absent is EMU_NO_VALUE, not 0: endpoint address 0 is a real OUT endpoint.
    CHECK_EQ(info.StatusEndpoint, EMU_NO_VALUE);
    CHECK(info.StatusEndpoint != 0u);
    CHECK_EQ(info.CaptureAltCount, 0u);
    CHECK_EQ(info.RenderAltCount, 1u);
    // A range has no enumerable membership, so the list is empty and the flag is
    // the only honest statement about the rates this device has.
    CHECK_EQ(info.RateCount, 0u);
    CHECK_EQ(info.RateFlags, EMU_RATES_CONTINUOUS);
    CHECK_EQ(info.MinRateHz, 8000u);
    CHECK_EQ(info.MaxRateHz, 192000u);
}

// State before anything has been confirmed: the panel must see four unknown
// controls, and each of them must be marked unknown rather than zero.
static void CaseStateFillUnknown(void)
{
    g_case = "state-fill-unknown";
    EMU_SETTINGS s;
    EMU_STATE_INFO info;

    EmuSettingsInit(&s);
    EmuIoctlFillState(&info, &s, EMU_WIRE_STATE_STARTED, 1, 0, 0,
                      (NTSTATUS)0);
    CHECK_EQ(info.Size, sizeof(EMU_STATE_INFO));
    CHECK_EQ(info.Version, EMU_IOCTL_VERSION);
    CHECK_EQ(info.ValidMask, 0u);
    CHECK_EQ(info.StateSequence, 0u);
    CHECK_EQ(info.RateHz, EMU_NO_VALUE);
    CHECK_EQ(info.ClockSource, EMU_NO_VALUE);
    CHECK_EQ(info.SpdifFormat, EMU_NO_VALUE);
    CHECK_EQ(info.SoftLimit, EMU_NO_VALUE);
    CHECK_EQ(info.LastOperationStatus, 0u);
    CHECK_EQ(info.PnpState, EMU_WIRE_STATE_STARTED);
    CHECK_EQ(info.StartCount, 1u);
    CHECK_EQ(info.StopCount, 0u);
    CHECK_EQ(info.RemoveCount, 0u);
    // The distinction the whole EMU_NO_VALUE business exists for: a zero here
    // would read as "0 Hz on the internal clock with the limiter off".
    CHECK(info.RateHz != 0u);
    CHECK(info.SoftLimit != 0u);
}

static void CaseStateFillKnown(void)
{
    g_case = "state-fill-known";
    static EMU_DEVICE_CAPS caps;
    EMU_SETTINGS s;
    EMU_STATE_INFO info;

    CHECK(Parse0404(&caps));
    EmuSettingsInit(&s);
    CHECK(EmuSettingsNoteRate(&s, &caps, 48000));
    CHECK(EmuSettingsNoteRate(&s, &caps, 96000));

    // Only the rate is confirmed, and only by notes the cache accepted; the
    // rejected ones below must not move the sequence either.
    CHECK(!EmuSettingsNoteRate(&s, &caps, 44101));
    CHECK(!EmuSettingsNoteRate(&s, &caps, 0));
    EmuIoctlFillState(&info, &s, EMU_WIRE_STATE_STARTED, 2, 1, 0,
                      (NTSTATUS)0);
    CHECK_EQ(info.ValidMask, EMU_STATE_VALID_RATE);
    CHECK_EQ(info.RateHz, 96000u);
    CHECK_EQ(info.StateSequence, 2u);
    CHECK_EQ(info.ClockSource, EMU_NO_VALUE);
    CHECK_EQ(info.SpdifFormat, EMU_NO_VALUE);
    CHECK_EQ(info.SoftLimit, EMU_NO_VALUE);
    CHECK_EQ(info.StartCount, 2u);
    CHECK_EQ(info.StopCount, 1u);

    // The three byte controls are written by the SET path once the hardware
    // trace defines it; until then a driver that has written nothing may not
    // claim them, so this fill shows what the mask does when they are set.
    s.ClockSource = 1;
    s.SpdifFormat = 2;
    s.SoftLimit = 1;
    EmuIoctlFillState(&info, &s, EMU_WIRE_STATE_STOPPED, 2, 1, 0,
                      STATUS_DEVICE_NOT_READY);
    CHECK_EQ(info.ValidMask, EMU_STATE_VALID_RATE | EMU_STATE_VALID_CLOCK_SOURCE |
                             EMU_STATE_VALID_SPDIF_FORMAT | EMU_STATE_VALID_SOFT_LIMIT);
    CHECK_EQ(info.ClockSource, 1u);
    CHECK_EQ(info.SpdifFormat, 2u);
    CHECK_EQ(info.SoftLimit, 1u);
    CHECK_EQ(info.StateSequence, 2u);            // unchanged: no new accepted note
    CHECK_EQ(info.PnpState, EMU_WIRE_STATE_STOPPED);
    // An NTSTATUS travels as a ULONG and must arrive with its error bit intact.
    CHECK_EQ((ULONG)info.LastOperationStatus, (ULONG)STATUS_DEVICE_NOT_READY);
    CHECK(info.LastOperationStatus != 0u);
    CHECK(!(NT_SUCCESS((NTSTATUS)info.LastOperationStatus)));

    // A released device leaves no evidence: this is the state after
    // EmuUsbRelease called EmuSettingsInit, and the sequence has to restart with
    // the session rather than carry on counting the previous one.
    EmuSettingsInit(&s);
    EmuIoctlFillState(&info, &s, EMU_WIRE_STATE_SURPRISE_REMOVED, 2, 1, 1,
                      (NTSTATUS)0);
    CHECK_EQ(info.ValidMask, 0u);
    CHECK_EQ(info.StateSequence, 0u);
    CHECK_EQ(info.RateHz, EMU_NO_VALUE);
    CHECK_EQ(info.PnpState, EMU_WIRE_STATE_SURPRISE_REMOVED);
    CHECK_EQ(info.RemoveCount, 1u);
}

// The contract is a byte layout as much as a set of numbers: the panel is a
// separate binary, compiled at a different time, and it reads these offsets.
// Pinning the ones a client actually depends on is what turns a reordered field
// into a build failure instead of a wrong number on screen.
static void CaseWireLayout(void)
{
    g_case = "wire-layout";

    CHECK_EQ(offsetof(EMU_CAPS_INFO, Size), 0u);
    CHECK_EQ(offsetof(EMU_CAPS_INFO, Version), 4u);
    CHECK_EQ(offsetof(EMU_CAPS_INFO, ImplementedMask), 8u);
    CHECK_EQ(offsetof(EMU_CAPS_INFO, SupportedMask), 12u);
    CHECK_EQ(offsetof(EMU_CAPS_INFO, RateCount), 48u);
    CHECK_EQ(offsetof(EMU_CAPS_INFO, RateFlags), 52u);
    CHECK_EQ(offsetof(EMU_CAPS_INFO, Rates), 56u);
    CHECK_EQ(offsetof(EMU_CAPS_INFO, ClockRateUnitId), 56u + 16u * 4u);
    CHECK_EQ(offsetof(EMU_CAPS_INFO, StatusEndpoint),
             sizeof(EMU_CAPS_INFO) - 4u);

    CHECK_EQ(offsetof(EMU_STATE_INFO, Size), 0u);
    CHECK_EQ(offsetof(EMU_STATE_INFO, ValidMask), 8u);
    CHECK_EQ(offsetof(EMU_STATE_INFO, StateSequence), 12u);
    CHECK_EQ(offsetof(EMU_STATE_INFO, RateHz), 16u);
    CHECK_EQ(offsetof(EMU_STATE_INFO, LastOperationStatus), 32u);
    CHECK_EQ(offsetof(EMU_STATE_INFO, PnpState), 36u);
    CHECK_EQ(offsetof(EMU_STATE_INFO, RemoveCount),
             sizeof(EMU_STATE_INFO) - 4u);

    // Both queries answer with their own header; a client that checks Size and
    // Version before reading anything else cannot be fooled by a newer driver.
    CHECK_EQ(sizeof(EMU_CAPS_INFO) % sizeof(ULONG), 0u);
    CHECK_EQ(sizeof(EMU_STATE_INFO) % sizeof(ULONG), 0u);
    CHECK_EQ(EMU_MAX_REPORTED_RATES, 16u);
}

// What the handler treats as "the instance can serve a panel request": only
// the started state, and only with a descriptor the parser really produced.
// Every other combination — never started, stopped, surprise-removed, removed,
// or started with the USB client already released — has to read as unusable,
// because each of them would otherwise be answered with a table describing a
// device the driver cannot currently reach.
static void CaseUsable(void)
{
    g_case = "usable";
    static EMU_DEVICE_CAPS caps;

    CHECK(Parse0404(&caps));
    CHECK_EQ(EmuIoctlIsDeviceUsable(EMU_WIRE_STATE_STARTED, &caps), TRUE);
    CHECK_EQ(EmuIoctlIsDeviceUsable(EMU_WIRE_STATE_NOT_STARTED, &caps), FALSE);
    CHECK_EQ(EmuIoctlIsDeviceUsable(EMU_WIRE_STATE_STOPPED, &caps), FALSE);
    CHECK_EQ(EmuIoctlIsDeviceUsable(EMU_WIRE_STATE_SURPRISE_REMOVED, &caps), FALSE);
    CHECK_EQ(EmuIoctlIsDeviceUsable(EMU_WIRE_STATE_REMOVED, &caps), FALSE);
    CHECK_EQ(EmuIoctlIsDeviceUsable(EMU_WIRE_STATE_STARTED,
                                    (const EMU_DEVICE_CAPS*)NULL), FALSE);
    CHECK_EQ(EmuIoctlIsDeviceUsable(EMU_WIRE_STATE_NOT_STARTED,
                                    (const EMU_DEVICE_CAPS*)NULL), FALSE);
}

int
main(void)
{
    printf("openEMUdriver EmuUSBAudio IOCTL contract test\n");
    printf("(contract: driver/emuusbaudio/Emu/emu_ioctl.h, same header the panel includes)\n");

    CaseForward();
    CaseNotReady();
    CaseBufferRules();
    CaseSetRules();
    CaseTestStreamRules();
    CaseUsable();
    CaseCapsFill0404();
    CaseCapsFillSparse();
    CaseStateFillUnknown();
    CaseStateFillKnown();
    CaseWireLayout();

    return TestResult();
}
