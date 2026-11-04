// EmuUSBAudio EP0 class-request builder test (M2 control path), user-mode.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Build and run:  cd driver && ./utest.sh
//
// The assertions are on the eight setup bytes, not on the helpers' return codes,
// because a field landing one byte off addresses a different control, and the
// device answers a well-formed wrong request by silently changing state.
//
// The expected bytes are the layout of the v1 WinUSB engine that already drives
// this hardware (src/emu_usb.cpp:311-360):
//
//   RequestType = out ? 0x21 : 0xA1
//   Request     = 0x01                       (CUR)
//   Value       = (cs << 8) | channel
//   Index       = (unitID << 8) | acIface
//   Length      = payload size
//
// Unit ids match the parser fixture (XU clock-rate at 0x0A, feature unit at
// 0x02); a real caller takes them from EmuCapsXuUnitId / EmuCapsFeatureUnitId.
#include <string.h>
#include "emu_ctrl.h"
#include "testharness.h"

int g_checks = 0;
int g_failures = 0;
const char* g_case = "";

#define UNIT_XU_CLOCK_RATE    0x0A
#define UNIT_XU_CLOCK_SOURCE  0x0B
#define UNIT_XU_DIGITAL_IO    0x0C
#define UNIT_XU_DEVICE_OPTS   0x0D
#define UNIT_FEATURE          0x02
#define AC_IFACE              0x00

// ---------------------------------------------------------------------------
// assertions
// ---------------------------------------------------------------------------

static void
ExpectSetup(
    _In_ const char* name,
    _In_ const EMU_CTRL_REQUEST* Request,
    _In_ UCHAR RequestType,
    _In_ ULONG UnitId,
    _In_ ULONG AcInterface,
    _In_ ULONG ControlSelector,
    _In_ ULONG Channel,
    _In_ ULONG PayloadLength
    )
{
    // Built in the same field order v1 assigns into WINUSB_SETUP_PACKET.
    unsigned char want[EMU_CTRL_SETUP_BYTES];

    want[0] = RequestType;                      // bmRequestType
    want[1] = EMU_CTRL_CS_CUR;                  // bRequest
    want[2] = (unsigned char)Channel;           // wValue, low
    want[3] = (unsigned char)ControlSelector;   // wValue, high
    want[4] = (unsigned char)AcInterface;       // wIndex, low
    want[5] = (unsigned char)UnitId;            // wIndex, high
    want[6] = (unsigned char)(PayloadLength & 0xFF);
    want[7] = (unsigned char)(PayloadLength >> 8);

    g_checks++;
    if (memcmp(Request->Bytes, want, EMU_CTRL_SETUP_BYTES) != 0) {
        g_failures++;
        printf("  FAIL [%s] %s: setup is %02X %02X %02X %02X %02X %02X %02X %02X, "
               "want %02X %02X %02X %02X %02X %02X %02X %02X\n",
               g_case, name,
               Request->Bytes[0], Request->Bytes[1], Request->Bytes[2], Request->Bytes[3],
               Request->Bytes[4], Request->Bytes[5], Request->Bytes[6], Request->Bytes[7],
               want[0], want[1], want[2], want[3],
               want[4], want[5], want[6], want[7]);
    }
    CHECK_EQ(Request->PayloadLength, PayloadLength);
}

// A rejected build has to be indistinguishable from "nothing was assembled": all
// zero, so bmRequestType names no class control at all.
static void
ExpectZeroed(
    _In_ const char* name,
    _In_ const EMU_CTRL_REQUEST* Request
    )
{
    unsigned char zero[sizeof(EMU_CTRL_REQUEST)];

    memset(zero, 0, sizeof(zero));
    g_checks++;
    if (memcmp(Request, zero, sizeof(*Request)) != 0) {
        g_failures++;
        printf("  FAIL [%s] %s: rejected request was left addressable "
               "(setup %02X %02X, payload length %lu)\n",
               g_case, name, Request->Bytes[0], Request->Bytes[1],
               (unsigned long)Request->PayloadLength);
    }
}

static void
FillWithSentinel(_Out_ EMU_CTRL_REQUEST* Request)
{
    memset(Request, 0xA5, sizeof(*Request));
}

static void
ExpectBytes4(_In_ const char* what, _In_reads_bytes_(4) const unsigned char* p,
             _In_ unsigned a, _In_ unsigned b, _In_ unsigned c, _In_ unsigned d)
{
    g_checks++;
    if (p[0] != a || p[1] != b || p[2] != c || p[3] != d) {
        g_failures++;
        printf("  FAIL [%s] %s: bytes are %02X %02X %02X %02X, "
               "want %02X %02X %02X %02X\n",
               g_case, what, p[0], p[1], p[2], p[3], a, b, c, d);
    }
}

static void
ExpectBytes2(_In_ const char* what, _In_reads_bytes_(2) const unsigned char* p,
             _In_ unsigned a, _In_ unsigned b)
{
    g_checks++;
    if (p[0] != a || p[1] != b) {
        g_failures++;
        printf("  FAIL [%s] %s: bytes are %02X %02X, want %02X %02X\n",
               g_case, what, p[0], p[1], a, b);
    }
}

// ---------------------------------------------------------------------------
// cases
// ---------------------------------------------------------------------------

static void
CaseSetCurWire(void)
{
    EMU_CTRL_REQUEST r;
    NTSTATUS status;

    g_case = "SET_CUR wire bytes";

    // Clock rate: what M2 sends on every sample-rate change.
    status = EmuCtrlBuildClockRateSet(&r, UNIT_XU_CLOCK_RATE, AC_IFACE, 44100);
    CheckStatus("clock-rate-44100-build", STATUS_SUCCESS, status);
    ExpectSetup("clock-rate-44100", &r, EMU_CTRL_REQUEST_TYPE_OUT,
                UNIT_XU_CLOCK_RATE, AC_IFACE, EMU_XU_CS_CLOCK_RATE_SELECT, 0, 1);
    CHECK_EQ(r.Payload[0], 0);

    status = EmuCtrlBuildClockRateSet(&r, UNIT_XU_CLOCK_RATE, AC_IFACE, 48000);
    CheckStatus("clock-rate-48000-build", STATUS_SUCCESS, status);
    ExpectSetup("clock-rate-48000", &r, EMU_CTRL_REQUEST_TYPE_OUT,
                UNIT_XU_CLOCK_RATE, AC_IFACE, EMU_XU_CS_CLOCK_RATE_SELECT, 0, 1);
    CHECK_EQ(r.Payload[0], 1);

    status = EmuCtrlBuildClockRateSet(&r, UNIT_XU_CLOCK_RATE, AC_IFACE, 192000);
    CheckStatus("clock-rate-192000-build", STATUS_SUCCESS, status);
    CHECK_EQ(r.Payload[0], 5);

    // Clock source (0 internal / 1 S/PDIF) on the clock-source unit.
    status = EmuCtrlBuildClockSourceSet(&r, UNIT_XU_CLOCK_SOURCE, AC_IFACE, 1);
    CheckStatus("clock-source-build", STATUS_SUCCESS, status);
    ExpectSetup("clock-source-external", &r, EMU_CTRL_REQUEST_TYPE_OUT,
                UNIT_XU_CLOCK_SOURCE, AC_IFACE, EMU_XU_CS_CLOCK_SOURCE, 0, 1);
    CHECK_EQ(r.Payload[0], 1);

    // Soft limiter on the device-options unit.
    status = EmuCtrlBuildSoftLimitSet(&r, UNIT_XU_DEVICE_OPTS, AC_IFACE, TRUE);
    CheckStatus("softlimit-on-build", STATUS_SUCCESS, status);
    ExpectSetup("softlimit-on", &r, EMU_CTRL_REQUEST_TYPE_OUT,
                UNIT_XU_DEVICE_OPTS, AC_IFACE, EMU_XU_CS_OPTION_SOFT_LIMIT, 0, 1);
    CHECK_EQ(r.Payload[0], 1);

    status = EmuCtrlBuildSoftLimitSet(&r, UNIT_XU_DEVICE_OPTS, AC_IFACE, FALSE);
    CheckStatus("softlimit-off-build", STATUS_SUCCESS, status);
    CHECK_EQ(r.Payload[0], 0);

    // S/PDIF output format on the digital I/O unit: 2 = AES.
    status = EmuCtrlBuildSpdifFormatSet(&r, UNIT_XU_DIGITAL_IO, AC_IFACE, 2);
    CheckStatus("digio-format-build", STATUS_SUCCESS, status);
    ExpectSetup("digio-format-aes", &r, EMU_CTRL_REQUEST_TYPE_OUT,
                UNIT_XU_DIGITAL_IO, AC_IFACE, EMU_XU_CS_DIGITAL_FORMAT, 0, 1);
    CHECK_EQ(r.Payload[0], 2);

    // Feature Unit volume: master control is channel index 1, payload 2 bytes.
    status = EmuCtrlBuildRequest(&r, TRUE, UNIT_FEATURE, AC_IFACE,
                                 EMU_FU_CS_VOLUME, 1, 2);
    CheckStatus("volume-set-build", STATUS_SUCCESS, status);
    ExpectSetup("volume-set-master", &r, EMU_CTRL_REQUEST_TYPE_OUT,
                UNIT_FEATURE, AC_IFACE, EMU_FU_CS_VOLUME, 1, 2);

    // Mute on the same unit: selector 1, one byte.
    status = EmuCtrlBuildRequest(&r, TRUE, UNIT_FEATURE, AC_IFACE,
                                 EMU_FU_CS_MUTE, 1, 1);
    CheckStatus("mute-set-build", STATUS_SUCCESS, status);
    ExpectSetup("mute-set-master", &r, EMU_CTRL_REQUEST_TYPE_OUT,
                UNIT_FEATURE, AC_IFACE, EMU_FU_CS_MUTE, 1, 1);
}

static void
CaseGetCurWire(void)
{
    EMU_CTRL_REQUEST r;
    NTSTATUS status;

    g_case = "GET_CUR wire bytes";

    // bmRequestType flips to 0xA1 and every other field keeps its place. v1
    // reads the S/PDIF state with these (getDigLock/getDigRate).
    status = EmuCtrlBuildRequest(&r, FALSE, UNIT_XU_DIGITAL_IO, AC_IFACE,
                                 EMU_XU_CS_DIGITAL_LOCK, 0, 1);
    CheckStatus("digio-lock-build", STATUS_SUCCESS, status);
    ExpectSetup("digio-lock", &r, EMU_CTRL_REQUEST_TYPE_IN,
                UNIT_XU_DIGITAL_IO, AC_IFACE, EMU_XU_CS_DIGITAL_LOCK, 0, 1);

    status = EmuCtrlBuildRequest(&r, FALSE, UNIT_XU_DIGITAL_IO, AC_IFACE,
                                 EMU_XU_CS_DIGITAL_RATE, 0, 4);
    CheckStatus("digio-rate-build", STATUS_SUCCESS, status);
    ExpectSetup("digio-rate", &r, EMU_CTRL_REQUEST_TYPE_IN,
                UNIT_XU_DIGITAL_IO, AC_IFACE, EMU_XU_CS_DIGITAL_RATE, 0, 4);

    // Volume read-back for a per-channel index, not the master one.
    status = EmuCtrlBuildRequest(&r, FALSE, UNIT_FEATURE, AC_IFACE,
                                 EMU_FU_CS_VOLUME, 2, 2);
    CheckStatus("volume-get-build", STATUS_SUCCESS, status);
    ExpectSetup("volume-get-channel2", &r, EMU_CTRL_REQUEST_TYPE_IN,
                UNIT_FEATURE, AC_IFACE, EMU_FU_CS_VOLUME, 2, 2);

    // A request with no data stage is legal and has to say so in wLength.
    status = EmuCtrlBuildRequest(&r, TRUE, UNIT_XU_CLOCK_RATE, AC_IFACE,
                                 EMU_XU_CS_CLOCK_RATE_SELECT, 0, 0);
    CheckStatus("zero-length-build", STATUS_SUCCESS, status);
    ExpectSetup("zero-length", &r, EMU_CTRL_REQUEST_TYPE_OUT,
                UNIT_XU_CLOCK_RATE, AC_IFACE, EMU_XU_CS_CLOCK_RATE_SELECT, 0, 0);
    CHECK_EQ(r.Payload[0], 0);

    // Widest payload the protocol uses: the 20-byte direct-monitoring structure.
    status = EmuCtrlBuildRequest(&r, TRUE, 0x0E, AC_IFACE, 2, 0,
                                 EMU_CTRL_PAYLOAD_MAX);
    CheckStatus("payload-max-build", STATUS_SUCCESS, status);
    CHECK_EQ(r.Bytes[6], EMU_CTRL_PAYLOAD_MAX);
    CHECK_EQ(r.Bytes[7], 0);
}

static void
CaseRateIndex(void)
{
    static const ULONG rates[EMU_XU_RATE_COUNT] = {
        44100, 48000, 88200, 96000, 176400, 192000
    };
    UCHAR index;
    ULONG i;

    g_case = "clock-rate index";

    for (i = 0; i < EMU_XU_RATE_COUNT; i++) {
        index = 0xA5;
        CHECK(EmuCtrlRateToIndex(rates[i], &index));
        CHECK_EQ(index, i);
        CHECK_EQ(EmuCtrlRateFromIndex(index), rates[i]);
    }

    // Rates the device has no index for: FALSE, and the out parameter untouched,
    // because index 0 would otherwise be sent as 44100.
    index = 0xA5;
    CHECK(!EmuCtrlRateToIndex(44000, &index));
    CHECK_EQ(index, 0xA5);
    CHECK(!EmuCtrlRateToIndex(0, &index));
    CHECK(!EmuCtrlRateToIndex(384000, &index));
    CHECK(!EmuCtrlRateToIndex(192001, &index));
    CHECK(!EmuCtrlRateToIndex(44100, NULL));

    // An out-of-range index reads back as 0 Hz, never as the first table entry.
    CHECK_EQ(EmuCtrlRateFromIndex(6), 0u);
    CHECK_EQ(EmuCtrlRateFromIndex(255), 0u);
}

static void
CaseDigitalRate(void)
{
    static const unsigned char allOnes[4]   = {0xFF, 0xFF, 0xFF, 0xFF};
    static const unsigned char fortyFour[4] = {0x00, 0x00, 0xAC, 0x44};
    unsigned char payload[4];

    g_case = "digital rate encoding";

    // Big-endian: the one field in this protocol that is not little-endian
    // (src/emu_usb.cpp getDigRate shifts b[0] << 24).
    EmuCtrlWriteDigitalRate(payload, 44100);
    ExpectBytes4("encode-44100", payload, 0x00, 0x00, 0xAC, 0x44);
    CHECK_EQ(EmuCtrlReadDigitalRate(payload), 44100u);

    EmuCtrlWriteDigitalRate(payload, 48000);
    ExpectBytes4("encode-48000", payload, 0x00, 0x00, 0xBB, 0x80);

    EmuCtrlWriteDigitalRate(payload, 192000);
    ExpectBytes4("encode-192000", payload, 0x00, 0x02, 0xEE, 0x00);

    // A rate that needs all four bytes keeps its byte order.
    EmuCtrlWriteDigitalRate(payload, 0x01020304);
    ExpectBytes4("encode-wide", payload, 0x01, 0x02, 0x03, 0x04);
    CHECK_EQ(EmuCtrlReadDigitalRate(payload), 0x01020304u);

    EmuCtrlWriteDigitalRate(payload, 0);
    ExpectBytes4("encode-zero", payload, 0x00, 0x00, 0x00, 0x00);

    CHECK_EQ(EmuCtrlReadDigitalRate(allOnes), 0xFFFFFFFFu);
    CHECK_EQ(EmuCtrlReadDigitalRate(fortyFour), 44100u);
}

static void
CaseVolumeEncoding(void)
{
    static const unsigned char minField[2] = {0x00, 0x80};
    static const unsigned char minus127[2] = {0x00, 0x81};   // 0x8100 = -32512
    static const unsigned char maxField[2] = {0xFF, 0x7F};
    unsigned char payload[2];

    g_case = "volume encoding";

    // SInt16 in 1/256 dB, little-endian. The device range is +12 dB to -127 dB;
    // the field extremes are checked too because the panel clamps to the former
    // and a sign bug shows up in the latter.
    EmuCtrlWriteVolume(payload, 3072);               // +12 dB
    ExpectBytes2("encode-plus12", payload, 0x00, 0x0C);
    CHECK_EQ(EmuCtrlReadVolume(payload), 3072);

    EmuCtrlWriteVolume(payload, 0);                  // 0 dB
    ExpectBytes2("encode-zero", payload, 0x00, 0x00);

    EmuCtrlWriteVolume(payload, (SHORT)-32512);      // -127 dB
    ExpectBytes2("encode-minus127", payload, 0x00, 0x81);
    CHECK_EQ(EmuCtrlReadVolume(payload), (SHORT)-32512);

    EmuCtrlWriteVolume(payload, (SHORT)-1);
    ExpectBytes2("encode-minus1", payload, 0xFF, 0xFF);
    CHECK_EQ(EmuCtrlReadVolume(payload), (SHORT)-1);

    EmuCtrlWriteVolume(payload, (SHORT)-32768);      // field minimum
    ExpectBytes2("encode-min", payload, 0x00, 0x80);
    CHECK_EQ(EmuCtrlReadVolume(payload), (SHORT)-32768);

    CHECK_EQ(EmuCtrlReadVolume(minField), (SHORT)-32768);
    CHECK_EQ(EmuCtrlReadVolume(minus127), (SHORT)-32512);
    CHECK_EQ(EmuCtrlReadVolume(maxField), 32767);
}

static void
CaseRejectsBadParameters(void)
{
    EMU_CTRL_REQUEST r;
    NTSTATUS status;

    g_case = "parameter guards";

    // Every guard runs against a sentinel-filled output: bytes left behind by a
    // rejected build could be sent as a real request.
    FillWithSentinel(&r);
    status = EmuCtrlBuildRequest(&r, TRUE, 0, AC_IFACE, EMU_FU_CS_VOLUME, 1, 2);
    CheckStatus("unit-zero", STATUS_INVALID_PARAMETER, status);
    ExpectZeroed("unit-zero", &r);

    FillWithSentinel(&r);
    status = EmuCtrlBuildRequest(&r, TRUE, 0x100, AC_IFACE, EMU_FU_CS_VOLUME, 1, 2);
    CheckStatus("unit-too-wide", STATUS_INVALID_PARAMETER, status);
    ExpectZeroed("unit-too-wide", &r);

    FillWithSentinel(&r);
    status = EmuCtrlBuildRequest(&r, TRUE, UNIT_FEATURE, 0x100, EMU_FU_CS_VOLUME, 1, 2);
    CheckStatus("iface-too-wide", STATUS_INVALID_PARAMETER, status);
    ExpectZeroed("iface-too-wide", &r);

    FillWithSentinel(&r);
    status = EmuCtrlBuildRequest(&r, TRUE, UNIT_FEATURE, AC_IFACE, 0, 1, 2);
    CheckStatus("selector-zero", STATUS_INVALID_PARAMETER, status);
    ExpectZeroed("selector-zero", &r);

    FillWithSentinel(&r);
    status = EmuCtrlBuildRequest(&r, TRUE, UNIT_FEATURE, AC_IFACE, 0x100, 1, 2);
    CheckStatus("selector-too-wide", STATUS_INVALID_PARAMETER, status);
    ExpectZeroed("selector-too-wide", &r);

    FillWithSentinel(&r);
    status = EmuCtrlBuildRequest(&r, TRUE, UNIT_FEATURE, AC_IFACE, EMU_FU_CS_VOLUME,
                                 0x100, 2);
    CheckStatus("channel-too-wide", STATUS_INVALID_PARAMETER, status);
    ExpectZeroed("channel-too-wide", &r);

    FillWithSentinel(&r);
    status = EmuCtrlBuildRequest(&r, TRUE, UNIT_FEATURE, AC_IFACE, EMU_FU_CS_VOLUME,
                                 1, EMU_CTRL_PAYLOAD_MAX + 1);
    CheckStatus("payload-too-long", STATUS_INVALID_PARAMETER, status);
    ExpectZeroed("payload-too-long", &r);

    status = EmuCtrlBuildRequest(NULL, TRUE, UNIT_FEATURE, AC_IFACE,
                                 EMU_FU_CS_VOLUME, 1, 2);
    CheckStatus("null-request", STATUS_INVALID_PARAMETER, status);

    // The wide-but-legal values must not be rejected along with them.
    status = EmuCtrlBuildRequest(&r, TRUE, 0xFF, 0xFF, 0xFF, 0xFF,
                                 EMU_CTRL_PAYLOAD_MAX);
    CheckStatus("all-fields-at-max", STATUS_SUCCESS, status);
    CHECK_EQ(r.Bytes[2], 0xFF);
    CHECK_EQ(r.Bytes[3], 0xFF);
    CHECK_EQ(r.Bytes[4], 0xFF);
    CHECK_EQ(r.Bytes[5], 0xFF);
}

static void
CaseRejectsBadValues(void)
{
    EMU_CTRL_REQUEST r;

    g_case = "value guards";

    // A rate the clock unit has no index for is refused, and nothing addressable
    // is left for a caller that forgot to check the status.
    CheckStatus("rate-unsupported", STATUS_NOT_SUPPORTED,
                EmuCtrlBuildClockRateSet(&r, UNIT_XU_CLOCK_RATE, AC_IFACE, 44000));
    ExpectZeroed("rate-unsupported", &r);

    CheckStatus("source-2", STATUS_INVALID_PARAMETER,
                EmuCtrlBuildClockSourceSet(&r, UNIT_XU_CLOCK_SOURCE, AC_IFACE, 2));
    ExpectZeroed("source-2", &r);

    CheckStatus("format-3", STATUS_INVALID_PARAMETER,
                EmuCtrlBuildSpdifFormatSet(&r, UNIT_XU_DIGITAL_IO, AC_IFACE, 3));
    ExpectZeroed("format-3", &r);

    CheckStatus("format-wide", STATUS_INVALID_PARAMETER,
                EmuCtrlBuildSpdifFormatSet(&r, UNIT_XU_DIGITAL_IO, AC_IFACE, 0x1FF));
    ExpectZeroed("format-wide", &r);

    // A bad unit id is caught by the wrapper too, not only by the raw builder.
    CheckStatus("softlimit-unit-zero", STATUS_INVALID_PARAMETER,
                EmuCtrlBuildSoftLimitSet(&r, 0, AC_IFACE, TRUE));
    ExpectZeroed("softlimit-unit-zero", &r);
}

int
main(void)
{
    printf("openEMUdriver EmuUSBAudio EP0 class-request builder test\n");
    printf("(builders: driver/emuusbaudio/Emu/emu_ctrl.cpp, same source as in the driver)\n");

    CaseSetCurWire();
    CaseGetCurWire();
    CaseRateIndex();
    CaseDigitalRate();
    CaseVolumeEncoding();
    CaseRejectsBadParameters();
    CaseRejectsBadValues();

    return TestResult();
}
