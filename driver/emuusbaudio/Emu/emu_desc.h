// EmuUSBAudio — UAC1 configuration-descriptor parser (M2).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Everything M2..M5 need to know about the device before a single byte is
// streamed: which interface is AudioControl, which alternate settings carry
// PCM, their rates/channels/endpoints, and the unit IDs of the E-MU Extension
// Units used for SET_CUR (docs/PROTOCOL.md).
//
// This source is compiled twice, unchanged: into emuusbaudio.sys (kernel) and
// into the user-mode test built by driver/utest.sh (plan test #1). It reads
// every field at its documented byte offset instead of casting onto descriptor
// structs, because the parser has to survive bogus bLength values and because
// <usb.h> is not usable from both translation-unit kinds.
#ifndef _EMU_DESC_H_
#define _EMU_DESC_H_

// One source, two targets. The kernel build defines EMU_KERNEL (kbuild.sh) and
// gets the types from <ntddk.h>; the user-mode test gets them from <windows.h>,
// with WIN32_NO_STATUS so that <ntstatus.h> can supply the STATUS_* codes
// without colliding with winerror.h.
#ifdef EMU_KERNEL
#include <ntddk.h>
#else
#ifndef WIN32_NO_STATUS
#define WIN32_NO_STATUS
#endif
#ifndef NOMINMAX
#define NOMINMAX              // <vector> and friends come with this build
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// <ntstatus.h> keeps its codes behind `#ifndef WIN32_NO_STATUS`, so the define
// has to go back off before including it. It uses NTSTATUS and NT_SUCCESS but
// defines neither, and winternl.h would supply them only by redeclaring a lot of
// unrelated kernel plumbing — declare the two things needed here instead.
#ifdef WIN32_NO_STATUS
#undef WIN32_NO_STATUS
#endif
#ifndef NT_SUCCESS
typedef LONG NTSTATUS;
typedef NTSTATUS* PNTSTATUS;
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif
#include <ntstatus.h>
#endif

// Bound every table: a descriptor from a device we have not seen before must
// not be able to make the driver write outside a fixed-size structure. Overflow
// is an error, never a silent truncation — a truncated table would let M3 build
// an endpoint map that does not match the device.
#define EMU_MAX_AUDIO_INTERFACES  8
#define EMU_MAX_TERMINALS        16
#define EMU_MAX_STREAM_ALTS      24
#define EMU_MAX_RATES_PER_ALT     8
#define EMU_MAX_XU_UNITS          8
#define EMU_MAX_FEATURE_UNITS     8

// How many distinct rates a caller may ask the parser to list. This is a report
// size, not a device limit: the union across the whole table is bounded only by
// EMU_MAX_STREAM_ALTS * EMU_MAX_RATES_PER_ALT, so the caller gets the lowest
// rates in ascending order plus EMU_RATES_TRUNCATED when there were more.
#define EMU_MAX_REPORTED_RATES   16

// "absent" marker for ULONG ids/endpoints. 0 cannot be used: endpoint address 0
// is a real OUT endpoint on CA0189 (render data EP 0x00 on 0404 USB).
#define EMU_NO_VALUE 0xFFFFFFFFu

// --- descriptor constants (USB 1.1 + USB Audio Class 1.0, Table A-5/A-6) -----
#define EMU_DT_CONFIGURATION        0x02
#define EMU_DT_INTERFACE            0x04
#define EMU_DT_ENDPOINT             0x05
#define EMU_DT_CS_INTERFACE         0x24
#define EMU_DT_CS_ENDPOINT          0x25

#define EMU_SUBTYPE_AC_HEADER          0x01
#define EMU_SUBTYPE_AC_INPUT_TERMINAL  0x02
#define EMU_SUBTYPE_AC_OUTPUT_TERMINAL 0x03
#define EMU_SUBTYPE_AC_FEATURE_UNIT    0x06
#define EMU_SUBTYPE_AC_EXTENSION_UNIT  0x08
#define EMU_SUBTYPE_AS_GENERAL         0x01
#define EMU_SUBTYPE_AS_FORMAT_TYPE_I   0x02
#define EMU_SUBTYPE_AS_FORMAT_TYPE_II  0x03
#define EMU_SUBTYPE_AS_FORMAT_TYPE_III 0x04

#define EMU_INTERFACE_CLASS_AUDIO      0x01
#define EMU_INTERFACE_CLASS_VENDOR     0xFF
#define EMU_AUDIO_SUBCLASS_CONTROL     0x01
#define EMU_AUDIO_SUBCLASS_STREAMING   0x02
#define EMU_AUDIO_SUBCLASS_MIDI_STREAMING 0x03

// Endpoint bmAttributes: bits 0-1 transfer type, 2-3 sync type, 4-5 usage type
// (the latter two only meaningful for isochronous endpoints).
#define EMU_EP_XFER_MASK      0x03
#define EMU_EP_XFER_ISOCHRONOUS 0x01
#define EMU_EP_ISO_MAX_PACKET 0x07FF
#define EMU_EP_SYNC_SHIFT     2
#define EMU_EP_SYNC_MASK      0x03
#define EMU_EP_USAGE_SHIFT    4
#define EMU_EP_USAGE_MASK     0x03
#define EMU_EP_USAGE_DATA           0
#define EMU_EP_USAGE_FEEDBACK       1
#define EMU_EP_USAGE_IMPLICIT_DATA  2
#define EMU_EP_USAGE_RESERVED_FB    3

// FORMAT_TYPE_I bSamFreqType: 0 means "continuous", the two 3-byte values that
// follow are the operating minimum and maximum rate.
#define EMU_ALT_CONTINUOUS_RATES 0x00000001

// Stream direction, indexed exactly like the 0=out / 1=in arrays in the v1
// WinUSB engine (src/emu_usb.h) so the two engines stay comparable.
#define EMU_STREAM_RENDER   0   // host -> device, data endpoint is OUT
#define EMU_STREAM_CAPTURE  1   // device -> host, data endpoint is IN

#define EMU_ROLE_UNKNOWN 0
#define EMU_ROLE_AC      1
#define EMU_ROLE_AS      2
#define EMU_ROLE_MIDI    3

// --- Extension Unit wire codes (E-MU CA0189, confirmed on 0404 USB) ----------
#define EMU_XU_CLOCK_RATE      0xE301
#define EMU_XU_CLOCK_SOURCE    0xE302
#define EMU_XU_DIGITAL_IO      0xE303
#define EMU_XU_DEVICE_OPTIONS  0xE304
#define EMU_XU_DIRECT_MONITOR  0xE305
#define EMU_XU_METERING        0xE306

// EXTENSION_UNIT layout: the class spec puts bNrSources between the unit ID and
// wExtensionCode; CA0189 puts wExtensionCode directly after the unit ID. Both
// are recognised, and which one matched is reported so a future descriptor that
// only *looks* like one of them is visible in the parse result.
#define EMU_XU_LAYOUT_SPEC     1
#define EMU_XU_LAYOUT_EMU      2

// --- one PCM alternate setting ----------------------------------------------
typedef struct _EMU_STREAM_ALT {
    ULONG InterfaceNumber;
    ULONG AltSetting;         // 1..n; the zero-bandwidth alt 0 is never recorded
    ULONG Direction;          // EMU_STREAM_RENDER / EMU_STREAM_CAPTURE
    ULONG TerminalId;         // bTerminalLink, resolved against the terminal table
    ULONG FormatTag;          // wFormatTag from AS_GENERAL (1 = PCM)
    ULONG Channels;
    ULONG SubFrameSize;       // bSubFrameSize, bytes per channel slot (3 = 24-in-32)
    ULONG BitResolution;      // bBitResolution
    ULONG RateFlags;          // EMU_ALT_CONTINUOUS_RATES or 0
    ULONG RateCount;          // discrete list length
    ULONG Rates[EMU_MAX_RATES_PER_ALT];
    ULONG ContinuousMinHz;
    ULONG ContinuousMaxHz;
    ULONG DataEndpoint;       // bEndpointAddress of the data ISO endpoint
    ULONG DataMaxPacket;      // wMaxPacketSize & 0x07FF
    ULONG DataAttributes;     // raw bmAttributes
    ULONG DataUsageType;      // EMU_EP_USAGE_* of the data endpoint
    ULONG FeedbackEndpoint;   // EMU_NO_VALUE when the stream has none
    ULONG FeedbackMaxPacket;
    ULONG FeedbackAttributes;
    ULONG DataInterval;       // bInterval of the data endpoint (frames)
} EMU_STREAM_ALT, *PEMU_STREAM_ALT;

typedef struct _EMU_XU_UNIT {
    ULONG UnitID;             // low byte of wIndex in SET_CUR
    ULONG ExtensionCode;      // EMU_XU_*
    ULONG InterfaceNumber;    // interface carrying this descriptor (AudioControl)
    ULONG Layout;             // EMU_XU_LAYOUT_*
    ULONG ControlSize;        // baControlSize, 0 when the layout did not carry it
} EMU_XU_UNIT, *PEMU_XU_UNIT;

typedef struct _EMU_FEATURE_UNIT {
    ULONG UnitID;
    ULONG SourceID;
    ULONG InterfaceNumber;
    ULONG ControlSize;
    ULONG ChannelCount;       // bmaControl entries after the master one
    ULONG MasterControls;     // bmaControls[0], e.g. 0x03 = VOLUME|MUTE
} EMU_FEATURE_UNIT, *PEMU_FEATURE_UNIT;

typedef struct _EMU_AUDIO_INTERFACE {
    ULONG InterfaceNumber;
    ULONG DeclaredClass;      // 0x01 or 0xFF on CA0189
    ULONG DeclaredSubclass;
    ULONG DeclaredProtocol;
    ULONG DeclaredNumEndpoints;
    ULONG Role;               // EMU_ROLE_*, derived from class-specific content
    ULONG StatusEndpoint;     // AC interrupt endpoint, EMU_NO_VALUE when absent
    ULONG StatusMaxPacket;
    ULONG StatusInterval;
} EMU_AUDIO_INTERFACE, *PEMU_AUDIO_INTERFACE;

#define EMU_CAPS_AC_FOUND           0x00000001
#define EMU_CAPS_VENDOR_CLASS       0x00000002  // >=1 audio iface declared 0xFF
#define EMU_CAPS_RENDER             0x00000004
#define EMU_CAPS_CAPTURE            0x00000008
#define EMU_CAPS_STATUS_ENDPOINT    0x00000010
#define EMU_CAPS_HAS_XU             0x00000020
#define EMU_CAPS_NON_PCM_FORMAT     0x00000040  // FORMAT_TYPE_II/III was skipped
#define EMU_CAPS_MIDI_INTERFACE     0x00000080  // present but unsupported in v2

// Roughly 2.5 KB with the limits above: pool-allocate it in StartDevice, do not
// put it in the device extension (that memory is nonpaged).
typedef struct _EMU_DEVICE_CAPS {
    ULONG ParsedFlags;
    ULONG ConfigurationValue;     // bConfigurationValue
    ULONG DeclaredTotalLength;    // wTotalLength
    ULONG BufferLength;           // bytes handed to the parser
    ULONG AudioInterfaceCount;
    EMU_AUDIO_INTERFACE AudioInterfaces[EMU_MAX_AUDIO_INTERFACES];
    ULONG AcInterfaceNumber;      // EMU_NO_VALUE when absent
    ULONG StatusEndpoint;         // EMU_NO_VALUE when absent
    ULONG StreamAltCount;
    EMU_STREAM_ALT StreamAlts[EMU_MAX_STREAM_ALTS];
    ULONG XuUnitCount;
    EMU_XU_UNIT XuUnits[EMU_MAX_XU_UNITS];
    ULONG FeatureUnitCount;
    EMU_FEATURE_UNIT FeatureUnits[EMU_MAX_FEATURE_UNITS];
    ULONG MaxChannels;            // widest recorded alternate setting
    ULONG MinRateHz;
    ULONG MaxRateHz;
} EMU_DEVICE_CAPS, *PEMU_DEVICE_CAPS;

// The bLength/type pair every descriptor starts with.
#define EMU_CONFIG_HEADER_MIN_LENGTH 9

// Parse one complete configuration descriptor block (the bytes returned by
// GET_DESCRIPTOR configuration, wTotalLength of them).
//
// Returns STATUS_SUCCESS only for a structurally sound block that yielded at
// least one usable AudioStreaming alternate setting. Malformed lengths,
// descriptors running past the buffer, contradictory endpoint directions and
// table overflow all fail, and the caller must then not use Caps at all.
// IRQL: any — this function only reads the buffer it is given.
NTSTATUS
EmuParseUsbConfigDescriptor(
    _In_reads_bytes_(BufferLength) const UCHAR* Descriptor,
    _In_ ULONG BufferLength,
    _Out_ PEMU_DEVICE_CAPS Caps
    );

// Unit ID of the Extension Unit with this wExtensionCode, EMU_NO_VALUE when the
// device does not have it. Feeds wIndex = (unitID << 8) | AcInterfaceNumber.
ULONG
EmuCapsXuUnitId(
    _In_ const EMU_DEVICE_CAPS* Caps,
    _In_ ULONG ExtensionCode
    );

// Number of recorded alternate settings for one direction.
ULONG
EmuCapsAltCount(
    _In_ const EMU_DEVICE_CAPS* Caps,
    _In_ ULONG Direction
    );

// Does one alternate setting carry this rate? A discrete list must name it
// exactly; a continuous range accepts everything between its operating minimum
// and maximum, inclusive. A rate of 0 is never supported: the parser rejects a
// zero rate in a discrete list and a zero minimum in a range, so an answer of
// TRUE here always came from a real descriptor value.
BOOLEAN
EmuAltSupportsRate(
    _In_ const EMU_STREAM_ALT* Alt,
    _In_ ULONG RateHz
    );

// The first recorded alternate setting of this direction and channel count that
// carries the rate, NULL when the device has none. Alts are searched in
// descriptor order, so the answer is deterministic for a given device.
//
// This is the lookup a stream start needs: interface number plus alt setting is
// what SelectInterface takes, and the endpoint fields come with it. It says
// nothing about formats (SubFrameSize, BitResolution) - the caller picks the
// alt it can actually fill.
_Ret_maybenull_ const EMU_STREAM_ALT*
EmuCapsFindAlt(
    _In_ const EMU_DEVICE_CAPS* Caps,
    _In_ ULONG Direction,
    _In_ ULONG Channels,
    _In_ ULONG RateHz
    );

// Does any recorded alternate setting carry this rate?
//
// Deliberately direction-agnostic: the CA0189 clock is one device-global
// control, so the only claim that can be made without knowing which streams the
// caller intends to open is "this rate appears somewhere". A caller that needs
// per-direction truth uses EmuCapsFindAlt.
BOOLEAN
EmuCapsSupportsRate(
    _In_ const EMU_DEVICE_CAPS* Caps,
    _In_ ULONG RateHz
    );

// EMU_RATES_CONTINUOUS: at least one alternate setting advertises a range
// rather than a list, so the list below is not the whole truth about the device.
// EMU_RATES_TRUNCATED: the union held more distinct rates than MaxRates, and
// what came back are the MaxRates lowest ones.
#define EMU_RATES_CONTINUOUS  0x00000001
#define EMU_RATES_TRUNCATED   0x00000002

// The discrete rates the device lists, deduplicated and ascending.
//
// Only discrete lists are collected: a continuous range has no enumerable
// membership, which is what EMU_RATES_CONTINUOUS is for. Returns the number of
// entries written to Rates, which never exceeds MaxRates.
ULONG
EmuCapsRateList(
    _In_ const EMU_DEVICE_CAPS* Caps,
    _Out_writes_(MaxRates) ULONG* Rates,
    _In_ ULONG MaxRates,
    _Out_ PULONG Flags
    );

#endif // _EMU_DESC_H_
