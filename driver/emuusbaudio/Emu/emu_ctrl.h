// EP0 class requests for the E-MU CA0189 (extension units and feature units).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Wire protocol from docs/PROTOCOL.md, cross-checked against the v1 WinUSB
// engine that runs on this hardware today (src/emu_usb.cpp:311-325) — the byte
// layout asserted in driver/tests/emu_ctrltest.cpp is that code's layout.
//
// Dual-target like Emu/emu_desc.h: pure arithmetic on bytes, compiled into the
// driver image and into the user-mode test. Nothing here touches USB or the
// kernel; sending a built request is Emu/emu_usb.cpp's job.
#ifndef _EMU_CTRL_H_
#define _EMU_CTRL_H_

#include "emu_desc.h"

// --- request shape -----------------------------------------------------------
//
// UAC1 class request, recipient = interface:
//
//   bmRequestType = 0x21 (host to device) / 0xA1 (device to host)
//   bRequest      = CUR / RANGE(MIN) / MAX / RES
//   wValue        = (control selector << 8) | channel number
//   wIndex        = (unit id << 8) | AudioControl interface number
//   wLength       = payload size
//
#define EMU_CTRL_REQUEST_TYPE_OUT  0x21
#define EMU_CTRL_REQUEST_TYPE_IN   0xA1

#define EMU_CTRL_CS_CUR            0x01

// Feature Unit selectors (UAC1 6.2.2.7). CA0189 puts the master control at
// channel 1, so channel 0 is a mix of nothing and is never addressed. The
// volume range is fixed by the device (+12 dB to -127 dB, see
// EmuCtrlWriteVolume), so GET_MIN/GET_MAX are not needed.
#define EMU_FU_CS_MUTE             0x01
#define EMU_FU_CS_VOLUME           0x02

// Extension Unit selectors, per wExtensionCode (docs/PROTOCOL.md table).
#define EMU_XU_CS_CLOCK_RATE_SELECT  0x03  // GET/SET, one rate index
#define EMU_XU_CS_CLOCK_SOURCE       0x02  // 0 = internal, 1 = S/PDIF
#define EMU_XU_CS_DIGITAL_RATE       0x02  // GET, 4 bytes, big-endian Hz
#define EMU_XU_CS_DIGITAL_LOCK       0x03  // GET
#define EMU_XU_CS_DIGITAL_FORMAT     0x05  // GET/SET, 0 none / 1 spdif / 2 aes
#define EMU_XU_CS_OPTION_SOFT_LIMIT  0x03  // GET/SET, 0 off / 1 on

// Widest payload the protocol uses (the direct-monitoring structure). A request
// longer than this is built in pieces by the caller, not by padding here.
#define EMU_CTRL_PAYLOAD_MAX       20
#define EMU_CTRL_SETUP_BYTES        8

typedef struct _EMU_CTRL_REQUEST {
    // Ready to copy into struct _URB_CONTROL_TRANSFER.SetupPacket.
    UCHAR Bytes[EMU_CTRL_SETUP_BYTES];
    ULONG PayloadLength;
    UCHAR Payload[EMU_CTRL_PAYLOAD_MAX];
} EMU_CTRL_REQUEST, *PEMU_CTRL_REQUEST;

// --- rate tables ------------------------------------------------------------
//
// CA0189 names sample rates on EP0 as a one-byte index, not as Hz: the clock-rate
// unit takes 0..5 for 44100..192000, and the digital-input unit reports Hz as a
// 4-byte big-endian number. Both are device-side encodings, so they belong here
// rather than in the descriptor parser, which only ever sees 3-byte LE rates.
#define EMU_XU_RATE_COUNT 6

// --- builders ---------------------------------------------------------------

// Assemble the 8 setup bytes and reserve PayloadLength bytes for the caller to
// fill with the writers below. Rejects anything that would not survive being
// truncated into a byte field, so a wrong unit id or channel cannot silently
// address a different control.
//
// PayloadLength 0 is legal (a request with no data stage).
//
// Every builder here follows one rule: a rejected build leaves *Request zeroed.
// bmRequestType 0 names no class control, so a caller that ignores the status
// gets a stalled EP0 rather than a SET_CUR on something else.
NTSTATUS
EmuCtrlBuildRequest(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ BOOLEAN ToDevice,
    _In_ ULONG UnitId,
    _In_ ULONG InterfaceNumber,
    _In_ ULONG ControlSelector,
    _In_ ULONG Channel,
    _In_ ULONG PayloadLength
    );

// One-byte clock-rate index. FALSE when the device has no such rate: the caller
// must not send a request for it.
BOOLEAN
EmuCtrlRateToIndex(
    _In_ ULONG RateHz,
    _Out_ PUCHAR Index
    );

// 0 when the index is out of range, so a stale byte from a failed GET cannot be
// read back as 44100 Hz.
ULONG
EmuCtrlRateFromIndex(
    _In_ UCHAR Index
    );

// Digital I/O rate: 4 bytes, big-endian (the one field in this protocol that is
// not little-endian).
VOID
EmuCtrlWriteDigitalRate(
    _Out_writes_bytes_(4) PUCHAR Payload,
    _In_ ULONG RateHz
    );

ULONG
EmuCtrlReadDigitalRate(
    _In_reads_bytes_(4) const UCHAR* Payload
    );

// Feature Unit volume: SInt16 in 1/256 dB, little-endian. The reference range
// on CA0189 is +12 dB (3072) down to -127 dB (-32512).
VOID
EmuCtrlWriteVolume(
    _Out_writes_bytes_(2) PUCHAR Payload,
    _In_ SHORT Two56thsOfDb
    );

SHORT
EmuCtrlReadVolume(
    _In_reads_bytes_(2) const UCHAR* Payload
    );

// --- convenience wrappers ---------------------------------------------------
//
// The four controls M2's panel and the IOCTLs actually change. Each builds the
// request and its payload in one call so that a caller cannot mix a selector
// from one unit with the payload format of another.

// Clock-rate unit (EMU_XU_CLOCK_RATE): select the clock by rate in Hz.
NTSTATUS
EmuCtrlBuildClockRateSet(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG UnitId,
    _In_ ULONG AcInterface,
    _In_ ULONG RateHz
    );

// Clock-source unit (EMU_XU_CLOCK_SOURCE): 0 internal, 1 S/PDIF.
NTSTATUS
EmuCtrlBuildClockSourceSet(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG UnitId,
    _In_ ULONG AcInterface,
    _In_ ULONG Source
    );

// Device-options unit (EMU_XU_DEVICE_OPTIONS), soft-limit selector.
NTSTATUS
EmuCtrlBuildSoftLimitSet(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG UnitId,
    _In_ ULONG AcInterface,
    _In_ BOOLEAN Enabled
    );

// Digital I/O unit (EMU_XU_DIGITAL_IO), output-format selector: 0 none,
// 1 S/PDIF, 2 AES.
NTSTATUS
EmuCtrlBuildSpdifFormatSet(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG UnitId,
    _In_ ULONG AcInterface,
    _In_ ULONG Format
    );

// Standard USB audio feature-unit control selectors (the E-MU feature unit
// is an ordinary one, unlike its vendor XU units).
#define EMU_FEATURE_CS_MUTE    0x01
#define EMU_FEATURE_CS_VOLUME  0x02
#define EMU_FEATURE_CTRL_MUTE  0x01

// Feature unit, master channel: SET_CUR mute = 0. The device keeps mute in
// its own nonvolatile state and carries no USB volume - the front-panel knob
// is the only gain - so a device that last met the stock panel muted stays
// silent through any stream until something writes this.
NTSTATUS
EmuCtrlBuildMuteOff(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG FeatureUnitId,
    _In_ ULONG AcInterface
    );

// Feature unit, given channel: SET_CUR volume, signed 1/256-dB, two bytes
// (0x0000 = 0 dB). The per-channel volume controls live on channels 1..N;
// the device keeps their values in nonvolatile memory, and a device last
// seen by the stock panel at -inf volume is silent through any stream
// despite the master unmute.
NTSTATUS
EmuCtrlBuildVolumeSet(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG FeatureUnitId,
    _In_ ULONG AcInterface,
    _In_ ULONG Channel,
    _In_ SHORT Volume
    );

#endif // _EMU_CTRL_H_
