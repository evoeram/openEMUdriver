// EmuUSBAudio control interface: contract implementation.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// No IRP and no PortCls call in this file, for the same reason Emu/emu_desc.cpp
// reads bytes instead of posting URBs: the part of the contract that can be
// silently wrong is the deciding and the serialising, and that is also the part
// provable without a device. The PnP handler does the copying and the forwarding
// and asks this module what to do; the user-mode test built by
// driver/utest.sh (unit "ioctl") runs the very same rules.
#include "emu_ioctl.h"

VOID
EmuIoctlClassify(
    _In_ const EMU_IOCTL_CALL* Call,
    _In_ BOOLEAN DeviceUsable,
    _Out_ PEMU_IOCTL_DECISION Decision
    )
{
    ULONG            required;
    EMU_IOCTL_ACTION action;

    Decision->Action = EmuIoctlForward;
    Decision->Status = STATUS_SUCCESS;
    Decision->OutputBytes = 0;

    if (Call->ControlCode == EMU_IOCTL_GET_CAPS) {
        action = EmuIoctlGetCaps;
        required = sizeof(EMU_CAPS_INFO);
    } else if (Call->ControlCode == EMU_IOCTL_GET_STATE) {
        action = EmuIoctlGetState;
        required = sizeof(EMU_STATE_INFO);
    } else if (Call->ControlCode == EMU_IOCTL_SET_RATE) {
        action = EmuIoctlSetRate;
        required = sizeof(EMU_SET_INPUT);
    } else if (Call->ControlCode == EMU_IOCTL_SET_CLOCK_SOURCE) {
        action = EmuIoctlSetClockSource;
        required = sizeof(EMU_SET_INPUT);
    } else if (Call->ControlCode == EMU_IOCTL_SET_SPDIF_FORMAT) {
        action = EmuIoctlSetSpdifFormat;
        required = sizeof(EMU_SET_INPUT);
    } else if (Call->ControlCode == EMU_IOCTL_SET_SOFT_LIMIT) {
        action = EmuIoctlSetSoftLimit;
        required = sizeof(EMU_SET_INPUT);
    } else {
        // Everything else belongs to somebody else and goes back untouched.
        // That is not a courtesy: the FDO is PortCls's, and KS/wdmaud drive it
        // through IRP_MJ_DEVICE_CONTROL with their own device type
        // (FILE_DEVICE_KS = 0x2f, where the two codes above are built on
        // FILE_DEVICE_UNKNOWN = 0x22), so a request that is not exactly one of
        // ours cannot be recognised as audio-class traffic and must reach
        // PcDispatchIrp. No function-code range is claimed for the same reason.
        return;
    }

    // The device question first. A panel that asks while the instance is stopped
    // or surprise-removed gets a refusal rather than a table describing a device
    // the driver cannot currently reach; the alternative is a reply that is
    // indistinguishable from a working one.
    if (DeviceUsable == FALSE) {
        Decision->Action = EmuIoctlReject;
        Decision->Status = STATUS_DEVICE_NOT_READY;
        return;
    }

    // The queries are buffered with no input stage at all: an input buffer
    // means the caller believes it is sending something this build does not
    // define, so it is refused instead of silently ignored - the same argument
    // that keeps unimplemented opcodes out of the header.
    if (action == EmuIoctlGetCaps || action == EmuIoctlGetState) {
        if (Call->InputBufferLength != 0) {
            Decision->Action = EmuIoctlReject;
            Decision->Status = STATUS_INVALID_PARAMETER;
            return;
        }
        if (Call->OutputBufferLength < required) {
            Decision->Action = EmuIoctlReject;
            Decision->Status = STATUS_BUFFER_TOO_SMALL;
            return;
        }
        Decision->Action = action;
        Decision->Status = STATUS_SUCCESS;
        Decision->OutputBytes = required;
        return;
    }

    // The SET operations: a fixed EMU_SET_INPUT stage in, the refreshed
    // EMU_STATE_INFO out. A larger input is read only as far as this build's
    // fields (a newer panel may append), a larger output is fine unread - the
    // same append-only compatibility the queries offer. The reply carries the
    // state so the panel sees the accepted value and LastOperationStatus in
    // one roundtrip instead of guessing and re-reading.
    if (Call->InputBufferLength < required) {
        Decision->Action = EmuIoctlReject;
        Decision->Status = STATUS_INVALID_PARAMETER;
        return;
    }
    if (Call->OutputBufferLength < sizeof(EMU_STATE_INFO)) {
        Decision->Action = EmuIoctlReject;
        Decision->Status = STATUS_BUFFER_TOO_SMALL;
        return;
    }

    Decision->Action = action;
    Decision->Status = STATUS_SUCCESS;
    Decision->OutputBytes = sizeof(EMU_STATE_INFO);
}

BOOLEAN
EmuIoctlIsDeviceUsable(
    _In_ ULONG State,
    _In_opt_ const EMU_DEVICE_CAPS* Caps
    )
{
    // Started is the only state in which the descriptor the reply would be
    // built from is live: the parser's output is released on stop, and a
    // surprise-removed instance has nothing to talk to even before its REMOVE
    // arrives. A non-NULL Caps without the started state is the leftover of a
    // stop in progress, and an instance that never started reports a zeroed
    // context — both read as unusable here.
    return State == EMU_WIRE_STATE_STARTED && Caps != NULL;
}

VOID
EmuIoctlFillCaps(
    _Out_ PEMU_CAPS_INFO Info,
    _In_ const EMU_DEVICE_CAPS* Caps,
    _In_ ULONG ImplementedMask
    )
{
    ULONG clockRate;
    ULONG clockSource;
    ULONG digitalIo;
    ULONG deviceOptions;
    ULONG supported;

    clockRate = EmuCapsXuUnitId(Caps, EMU_XU_CLOCK_RATE);
    clockSource = EmuCapsXuUnitId(Caps, EMU_XU_CLOCK_SOURCE);
    digitalIo = EmuCapsXuUnitId(Caps, EMU_XU_DIGITAL_IO);
    deviceOptions = EmuCapsXuUnitId(Caps, EMU_XU_DEVICE_OPTIONS);

    // SupportedMask is a statement about the descriptor, not about this build:
    // an operation is listed when the Extension Unit that would carry it is on
    // the device. Whether the driver can answer it is ImplementedMask's job, and
    // the panel may offer a control only set in both. GET_STREAM_STATS stays
    // clear here as well as there - until M4 has streams there is nothing for it
    // to describe, on any device.
    supported = EMU_OP_GET_CAPS | EMU_OP_GET_STATE;
    if (clockRate != EMU_NO_VALUE) {
        supported |= EMU_OP_SET_RATE;
    }
    if (clockSource != EMU_NO_VALUE) {
        supported |= EMU_OP_SET_CLOCK_SOURCE;
    }
    if (digitalIo != EMU_NO_VALUE) {
        supported |= EMU_OP_SET_SPDIF_FORMAT;
    }
    if (deviceOptions != EMU_NO_VALUE) {
        supported |= EMU_OP_SET_SOFT_LIMIT;
    }

    Info->Size = sizeof(EMU_CAPS_INFO);
    Info->Version = EMU_IOCTL_VERSION;
    Info->ImplementedMask = ImplementedMask;
    Info->SupportedMask = supported;
    Info->ParsedFlags = Caps->ParsedFlags;
    Info->ConfigurationValue = Caps->ConfigurationValue;
    Info->AcInterfaceNumber = Caps->AcInterfaceNumber;
    Info->RenderAltCount = EmuCapsAltCount(Caps, EMU_STREAM_RENDER);
    Info->CaptureAltCount = EmuCapsAltCount(Caps, EMU_STREAM_CAPTURE);
    Info->MaxChannels = Caps->MaxChannels;
    Info->MinRateHz = Caps->MinRateHz;
    Info->MaxRateHz = Caps->MaxRateHz;
    // Zeroed before the list is written: a buffered reply hands the caller every
    // byte of the structure, so entries past RateCount that were never assigned
    // would be kernel pool contents, and pool contents are not ours to publish.
    // A rate of 0 is not a value the parser can produce, so the padding cannot
    // be mistaken for a rate the device has.
    for (ULONG i = 0; i < EMU_MAX_REPORTED_RATES; i++) {
        Info->Rates[i] = 0;
    }
    Info->RateCount = EmuCapsRateList(Caps,
                                      Info->Rates,
                                      EMU_MAX_REPORTED_RATES,
                                      &Info->RateFlags);

    // The unit ids are the low byte of wIndex for a class request. Reported even
    // when this build has no operation that uses them (direct monitor, metering)
    // so the panel can tell "the hardware has no metering" from "the driver has
    // not been taught the metering request yet" - the first is EMU_NO_VALUE here.
    Info->ClockRateUnitId = clockRate;
    Info->ClockSourceUnitId = clockSource;
    Info->DigitalIoUnitId = digitalIo;
    Info->DeviceOptionsUnitId = deviceOptions;
    Info->DirectMonitorUnitId = EmuCapsXuUnitId(Caps, EMU_XU_DIRECT_MONITOR);
    Info->MeteringUnitId = EmuCapsXuUnitId(Caps, EMU_XU_METERING);

    Info->FeatureUnitCount = Caps->FeatureUnitCount;
    // Volume and mute are addressed through the first Feature Unit, which is the
    // one the 0404 firmware expects; a device with several reports the count so
    // the panel does not assume its FU is the one this number names.
    Info->FeatureUnitId = Caps->FeatureUnitCount > 0
                              ? Caps->FeatureUnits[0].UnitID
                              : EMU_NO_VALUE;
    Info->FeatureMasterControls = Caps->FeatureUnitCount > 0
                                      ? Caps->FeatureUnits[0].MasterControls
                                      : EMU_NO_VALUE;
    Info->StatusEndpoint = Caps->StatusEndpoint;
}

VOID
EmuIoctlFillState(
    _Out_ PEMU_STATE_INFO Info,
    _In_ const EMU_SETTINGS* Settings,
    _In_ ULONG PnpState,
    _In_ ULONG StartCount,
    _In_ ULONG StopCount,
    _In_ ULONG RemoveCount,
    _In_ NTSTATUS LastOperationStatus
    )
{
    ULONG valid = 0;

    // ValidMask is derived from the cache rather than carried in it: the marker
    // and the mask are the same fact stated twice, and two places can disagree.
    // A cleared bit means the field below it is EMU_NO_VALUE, i.e. nobody ever
    // read or wrote that control - which on hardware where GET_CUR stalls is the
    // normal condition right after a start, not a failure.
    if (Settings->RateHz != EMU_NO_VALUE) {
        valid |= EMU_STATE_VALID_RATE;
    }
    if (Settings->ClockSource != EMU_NO_VALUE) {
        valid |= EMU_STATE_VALID_CLOCK_SOURCE;
    }
    if (Settings->SpdifFormat != EMU_NO_VALUE) {
        valid |= EMU_STATE_VALID_SPDIF_FORMAT;
    }
    if (Settings->SoftLimit != EMU_NO_VALUE) {
        valid |= EMU_STATE_VALID_SOFT_LIMIT;
    }

    Info->Size = sizeof(EMU_STATE_INFO);
    Info->Version = EMU_IOCTL_VERSION;
    Info->ValidMask = valid;
    Info->StateSequence = Settings->Sequence;
    Info->RateHz = Settings->RateHz;
    Info->ClockSource = Settings->ClockSource;
    Info->SpdifFormat = Settings->SpdifFormat;
    Info->SoftLimit = Settings->SoftLimit;
    Info->LastOperationStatus = (ULONG)LastOperationStatus;
    Info->PnpState = PnpState;
    Info->StartCount = StartCount;
    Info->StopCount = StopCount;
    Info->RemoveCount = RemoveCount;
}
