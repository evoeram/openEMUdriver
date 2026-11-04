// EmuUSBAudio — cached state of the E-MU control panel, implementation.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The whole module is one rule: nothing enters the cache that the parsed
// descriptor contradicts. Everything else about a control - the request bytes,
// the payload width - is Emu/emu_ctrl.cpp's business, and talking to the device
// is Emu/emu_usb.cpp's.
#include "emu_state.h"

VOID
EmuSettingsInit(_Out_ PEMU_SETTINGS Settings)
{
    Settings->RateHz = EMU_NO_VALUE;
    Settings->ClockSource = EMU_NO_VALUE;
    Settings->SpdifFormat = EMU_NO_VALUE;
    Settings->SoftLimit = EMU_NO_VALUE;
    Settings->Sequence = 0;
}

BOOLEAN
EmuSettingsNoteRate(
    _Inout_ PEMU_SETTINGS Settings,
    _In_ const EMU_DEVICE_CAPS* Caps,
    _In_ ULONG RateHz
    )
{
    if (!EmuCapsSupportsRate(Caps, RateHz)) {
        return FALSE;
    }
    Settings->RateHz = RateHz;
    Settings->Sequence++;
    return TRUE;
}
