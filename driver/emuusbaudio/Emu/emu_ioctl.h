// EmuUSBAudio control interface: the IOCTL contract shared by driver and panel.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// docs/V2_PLAN.md ("IOCTL-контракт") states the rule this file exists to enforce:
// user mode never names a unit id, a control selector or a payload length. It
// asks for one of a closed set of operations, and the driver alone decides which
// CA0189 request that operation corresponds to. So this header is the whole of
// what the panel may send, and everything it needs to interpret the answer.
//
// Dual-target like Emu/emu_desc.h: the same bytes are compiled into
// emuusbaudio.sys and read by the user-mode test (driver/utest.sh ioctl) and,
// from M7 on, by the panel. Keeping the layout here rather than in two headers
// is what makes "the panel read a field the driver never wrote" impossible to
// ship by accident.
#ifndef _EMU_IOCTL_H_
#define _EMU_IOCTL_H_

#include "emu_desc.h"      // ULONG / NTSTATUS / EMU_NO_VALUE, both targets
#include "emu_state.h"     // EMU_SETTINGS, the cache a state reply reads

#ifndef EMU_KERNEL
// <windows.h> with WIN32_LEAN_AND_MEAN does not pull in the IOCTL macros, and
// the kernel gets them from <ntddk.h>. Both define CTL_CODE identically.
#include <winioctl.h>
#endif

// --- the device interface ---------------------------------------------------
//
// Registered by the driver in StartDevice and advertised by the INF's
// .Interfaces section; the panel opens it and talks to the FDO through it.
// Generated for this project (no vendor namespace to inherit).
// {6DC68D18-D4C8-4DAC-A456-6CA921953BE8}
DEFINE_GUID(GUID_DEVINTERFACE_EMUUSBAUDIO,
            0x6dc68d18, 0xd4c8, 0x4dac,
            0xa4, 0x56, 0x6c, 0xa9, 0x21, 0x95, 0x3b, 0xe8);

// Every structure below starts with Size and Version. A panel that recognises
// neither bails out before it reads a field, so growing a structure is a
// compatible change as long as fields are only appended. Bumping Version
// together with Size is how the driver says "the fields you are reading mean
// something else now".
#define EMU_IOCTL_VERSION  1

// --- operations -------------------------------------------------------------
//
// FILE_DEVICE_UNKNOWN in the company-defined range (0x800 and up), buffered, and
// read access for the queries. There are no write operations yet: the SET path
// is deliberately not defined until the hardware trace says what CA0189 accepts,
// so a panel built today cannot send a request that would retune the device.
#define EMU_IOCTL_BASE          FILE_DEVICE_UNKNOWN

#define EMU_IOCTL_GET_CAPS      CTL_CODE(EMU_IOCTL_BASE, 0x800, METHOD_BUFFERED, \
                                         FILE_READ_ACCESS)
#define EMU_IOCTL_GET_STATE     CTL_CODE(EMU_IOCTL_BASE, 0x801, METHOD_BUFFERED, \
                                         FILE_READ_ACCESS)

// The four write operations of the CA0189 control plane (stage: SET path).
// FILE_WRITE_ACCESS distinguishes them from the queries of the same function
// code — a SET is a different control code number, exactly as the classify
// test insists. Every SET carries an EMU_SET_INPUT and answers with the
// refreshed EMU_STATE_INFO, so the panel sees the accepted value and the
// operation status in one roundtrip.
#define EMU_IOCTL_SET_RATE          CTL_CODE(EMU_IOCTL_BASE, 0x802, METHOD_BUFFERED, \
                                             FILE_WRITE_ACCESS)
#define EMU_IOCTL_SET_CLOCK_SOURCE  CTL_CODE(EMU_IOCTL_BASE, 0x803, METHOD_BUFFERED, \
                                             FILE_WRITE_ACCESS)
#define EMU_IOCTL_SET_SPDIF_FORMAT  CTL_CODE(EMU_IOCTL_BASE, 0x804, METHOD_BUFFERED, \
                                             FILE_WRITE_ACCESS)
#define EMU_IOCTL_SET_SOFT_LIMIT    CTL_CODE(EMU_IOCTL_BASE, 0x805, METHOD_BUFFERED, \
                                             FILE_WRITE_ACCESS)

// The SET input stage: the value only, never a unit id, control selector or
// length — those stay the driver's business. Size/Version first, like every
// structure here, so a panel built against a newer driver is still readable.
// Value meaning is per operation: rate in Hz, clock source 0/1, digital
// output format 0/1/2, soft limit 0/1.
typedef struct _EMU_SET_INPUT {
    ULONG Size;                    // sizeof(EMU_SET_INPUT)
    ULONG Version;                 // EMU_IOCTL_VERSION
    ULONG Value;
} EMU_SET_INPUT, *PEMU_SET_INPUT;

// Layout discipline shared with the two replies: ULONG-only, no pointer, no
// 64-bit scalar, so the 32-bit and 64-bit panel see the same bytes.
C_ASSERT(sizeof(EMU_SET_INPUT) == 3 * sizeof(ULONG));

// Operation bits. SupportedMask says what the device's descriptor could back;
// ImplementedMask says what this driver build answers. The panel may only offer
// a control that is set in both - one number each, and neither is a guess about
// the other.
#define EMU_OP_GET_CAPS          0x00000001
#define EMU_OP_GET_STATE         0x00000002
#define EMU_OP_SET_RATE          0x00000004
#define EMU_OP_SET_CLOCK_SOURCE  0x00000008
#define EMU_OP_SET_SPDIF_FORMAT  0x00000010
#define EMU_OP_SET_SOFT_LIMIT    0x00000020
#define EMU_OP_GET_STREAM_STATS  0x00000040

// This build answers all four SET operations as well. The hardware verdict
// that unlocked them: the v1 WinUSB engine has driven these exact requests on
// the same firmware for years, and the wire layout is asserted byte-for-byte
// in driver/utest.sh ctrl.
#define EMU_OP_IMPLEMENTED       (EMU_OP_GET_CAPS | EMU_OP_GET_STATE | \
                                  EMU_OP_SET_RATE | EMU_OP_SET_CLOCK_SOURCE | \
                                  EMU_OP_SET_SPDIF_FORMAT | EMU_OP_SET_SOFT_LIMIT)

// ValidMask bits of the state reply: which of the four controls the driver
// actually confirmed. A clear bit means the field holds EMU_NO_VALUE, and it is
// the difference between "the soft limit is off" and "nobody has ever read or
// written the soft limit" - which on this hardware, where GET_CUR stalls, is the
// normal condition rather than the exception.
#define EMU_STATE_VALID_RATE         0x00000001
#define EMU_STATE_VALID_CLOCK_SOURCE 0x00000002
#define EMU_STATE_VALID_SPDIF_FORMAT 0x00000004
#define EMU_STATE_VALID_SOFT_LIMIT   0x00000008

// PnpState values on the wire. They mirror EMU_STATE in Emu/emu.h, which is
// checked against them at compile time; the numbers are spelled out here so a
// panel does not depend on an enum it cannot include.
#define EMU_WIRE_STATE_NOT_STARTED       0
#define EMU_WIRE_STATE_STARTED           1
#define EMU_WIRE_STATE_STOPPED           2
#define EMU_WIRE_STATE_SURPRISE_REMOVED  3
#define EMU_WIRE_STATE_REMOVED           4

// --- replies ----------------------------------------------------------------
//
// All ULONG, so the layout is the same in the 32-bit and 64-bit panel and in the
// driver: no pointer, no 64-bit scalar, no padding to align.

typedef struct _EMU_CAPS_INFO {
    ULONG Size;                    // sizeof(EMU_CAPS_INFO)
    ULONG Version;                 // EMU_IOCTL_VERSION
    ULONG ImplementedMask;         // EMU_OP_* this build answers
    ULONG SupportedMask;           // EMU_OP_* the descriptor could back
    ULONG ParsedFlags;             // EMU_CAPS_* as the parser left them
    ULONG ConfigurationValue;      // bConfigurationValue of the read descriptor
    ULONG AcInterfaceNumber;       // the interface every class request names in wIndex
    ULONG RenderAltCount;
    ULONG CaptureAltCount;
    ULONG MaxChannels;
    ULONG MinRateHz;
    ULONG MaxRateHz;
    ULONG RateCount;               // entries written in Rates
    ULONG RateFlags;               // EMU_RATES_*
    ULONG Rates[EMU_MAX_REPORTED_RATES];
    ULONG ClockRateUnitId;         // EMU_NO_VALUE when the device has no such unit
    ULONG ClockSourceUnitId;
    ULONG DigitalIoUnitId;
    ULONG DeviceOptionsUnitId;
    ULONG DirectMonitorUnitId;
    ULONG MeteringUnitId;
    ULONG FeatureUnitCount;
    ULONG FeatureUnitId;           // the first one; volume/mute addresses it
    ULONG FeatureMasterControls;   // bmaControls[0], 0x03 = VOLUME|MUTE
    ULONG StatusEndpoint;          // AudioControl interrupt EP, EMU_NO_VALUE if none
} EMU_CAPS_INFO, *PEMU_CAPS_INFO;

typedef struct _EMU_STATE_INFO {
    ULONG Size;                    // sizeof(EMU_STATE_INFO)
    ULONG Version;                 // EMU_IOCTL_VERSION
    ULONG ValidMask;               // EMU_STATE_VALID_*
    ULONG StateSequence;           // bumped by every accepted cache note
    ULONG RateHz;                  // EMU_NO_VALUE until confirmed
    ULONG ClockSource;
    ULONG SpdifFormat;
    ULONG SoftLimit;
    ULONG LastOperationStatus;     // NTSTATUS of the last SET attempted, 0 = none yet
    ULONG PnpState;                // EMU_WIRE_STATE_*
    ULONG StartCount;
    ULONG StopCount;
    ULONG RemoveCount;
} EMU_STATE_INFO, *PEMU_STATE_INFO;

// --- what the driver makes of a request -------------------------------------

typedef struct _EMU_IOCTL_CALL {
    ULONG ControlCode;
    ULONG InputBufferLength;
    ULONG OutputBufferLength;
} EMU_IOCTL_CALL, *PEMU_IOCTL_CALL;

typedef enum _EMU_IOCTL_ACTION {
    // Not one of ours: the FDO belongs to PortCls, which serves KS and the audio
    // stack through it, so anything else has to be forwarded untouched.
    EmuIoctlForward = 0,
    EmuIoctlGetCaps,
    EmuIoctlGetState,
    EmuIoctlSetRate,
    EmuIoctlSetClockSource,
    EmuIoctlSetSpdifFormat,
    EmuIoctlSetSoftLimit,
    EmuIoctlReject       // ours, but refused; Status says why
} EMU_IOCTL_ACTION;

typedef struct _EMU_IOCTL_DECISION {
    EMU_IOCTL_ACTION Action;
    NTSTATUS         Status;             // meaningful for EmuIoctlReject
    ULONG            OutputBytes;        // what the reply needs, 0 when refusing
} EMU_IOCTL_DECISION, *PEMU_IOCTL_DECISION;

// Decide what to do with a device-control request without touching the IRP.
//
// DeviceUsable is the driver's own "started with a parsed descriptor" answer:
// while it is FALSE every one of our codes is refused, because the alternative
// is replying with a table that describes a device the driver cannot currently
// reach. Buffer rules are the METHOD_BUFFERED ones: no input for a query, and an
// output buffer at least as large as the structure the reply would fill (the
// panel is allowed to hand over a larger one, so a newer driver can answer an
// older client).
VOID
EmuIoctlClassify(
    _In_ const EMU_IOCTL_CALL* Call,
    _In_ BOOLEAN DeviceUsable,
    _Out_ PEMU_IOCTL_DECISION Decision
    );

// The rule behind DeviceUsable, so the handler cannot grow its own: an instance
// answers only in the started state with a parsed descriptor. State is an
// EMU_WIRE_STATE_* value, not the driver's EMU_STATE enum — that one lives in
// the kernel-only Emu/emu.h, whose compile-time asserts pin the two tables to
// the same numbers; here the started state is named in panel terms.
BOOLEAN
EmuIoctlIsDeviceUsable(
    _In_ ULONG State,
    _In_opt_ const EMU_DEVICE_CAPS* Caps
    );

// Fill a caps reply from a Caps table that came out of a successful parse.
// ImplementedMask is passed in rather than derived, so the number the panel sees
// is what the driver dispatches, not what the descriptor happens to contain.
VOID
EmuIoctlFillCaps(
    _Out_ PEMU_CAPS_INFO Info,
    _In_ const EMU_DEVICE_CAPS* Caps,
    _In_ ULONG ImplementedMask
    );

// Fill a state reply. Everything that is not confirmed arrives as EMU_NO_VALUE,
// and ValidMask says so field by field.
VOID
EmuIoctlFillState(
    _Out_ PEMU_STATE_INFO Info,
    _In_ const EMU_SETTINGS* Settings,
    _In_ ULONG PnpState,
    _In_ ULONG StartCount,
    _In_ ULONG StopCount,
    _In_ ULONG RemoveCount,
    _In_ NTSTATUS LastOperationStatus
    );

#endif // _EMU_IOCTL_H_
