// EmuUSBAudio — cached state of the E-MU control panel.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// M2's four device-global controls: the clock rate, the clock source, the
// digital output format and the analog soft limit. Selectors, payload widths and
// legal values come from docs/PROTOCOL.md, cross-checked against the v1 engine
// (src/emu_usb.cpp, src/asio_emu.cpp).
//
// Dual-target like Emu/emu_desc.h: arithmetic on plain ULONGs, compiled into
// emuusbaudio.sys and into the user-mode test built by driver/utest.sh. Keeping
// the rules here means a rejected value is refused the same way whether it came
// from a GET that stalled or from an IOCTL, and the rule itself is testable
// without a device.
#ifndef _EMU_STATE_H_
#define _EMU_STATE_H_

#include "emu_desc.h"

// EMU_NO_VALUE means "never established", not "off" and not zero. CA0189 does
// not answer GET_CUR on this hardware (docs/PROTOCOL.md, recorded by v1), so a
// field is only allowed to hold a value the driver actually saw: read back
// successfully, or written by its own SET_CUR. Until then the panel has to show
// "unknown" rather than a guess, and a guess is what a zero-initialised
// structure would silently offer (0 Hz, internal clock, no soft limit) -
// EmuSettingsInit is what keeps that from happening.
typedef struct _EMU_SETTINGS {
    ULONG RateHz;        // the rate in effect; 0 is never a legal value
    ULONG ClockSource;   // 0 = internal, 1 = S/PDIF
    ULONG SpdifFormat;   // 0 = none, 1 = S/PDIF, 2 = AES
    ULONG SoftLimit;     // 0 = off, 1 = on
    // Bumped by every note this cache accepted. A reader that polled the state
    // twice compares this against the control it re-read, so a value that
    // changed and came back to the same number is still visible as a change.
    ULONG Sequence;
} EMU_SETTINGS, *PEMU_SETTINGS;

// Marks everything unknown. Called on a fresh start and again whenever the USB
// client releases, because after a stop the driver has no evidence left about
// what the device was last set to. The sequence restarts at zero with the
// evidence: a reader that kept a sequence from the previous life would compare
// numbers that do not describe the same session, which is why GET_STATE also
// reports the start count.
VOID
EmuSettingsInit(
    _Out_ PEMU_SETTINGS Settings
    );

// Record the rate the device is now running at.
//
// FALSE and nothing written when the descriptor contradicts the value, which is
// the only way this driver has of catching a stale or bogus byte: the clock-rate
// control answers with a one-byte index, and an index outside the table decodes
// to 0 Hz. A rejected note must never overwrite a rate that was known, because
// the failed read is the more likely explanation for the new value.
//
// The caller supplies a Caps that came out of a successful parse.
// IRQL: any - this only reads a table and writes one ULONG.
BOOLEAN
EmuSettingsNoteRate(
    _Inout_ PEMU_SETTINGS Settings,
    _In_ const EMU_DEVICE_CAPS* Caps,
    _In_ ULONG RateHz
    );

#endif // _EMU_STATE_H_
