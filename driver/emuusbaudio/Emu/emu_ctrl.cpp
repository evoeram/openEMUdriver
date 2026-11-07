// EP0 class requests for the E-MU CA0189 — implementation.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Byte offsets and values mirror src/emu_usb.cpp:311-325, the v1 WinUSB path
// that has driven these devices for years; docs/PROTOCOL.md records where each
// field comes from. Kept free of USB and kernel calls so the same object code
// can be asserted in user mode.
#include "emu_ctrl.h"

// Clock-rate unit's one-byte index, in the order the device reports it.
static const ULONG g_EmuXuRates[EMU_XU_RATE_COUNT] = {
    44100, 48000, 88200, 96000, 176400, 192000
};

static NTSTATUS
EmuCtrlBuildInto(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ UCHAR RequestType,
    _In_ ULONG UnitId,
    _In_ ULONG InterfaceNumber,
    _In_ ULONG ControlSelector,
    _In_ ULONG Channel,
    _In_ ULONG PayloadLength
    )
{
    if (Request == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    // Zero before validating, so every failure path leaves the same thing: a
    // request that names no control at all. bmRequestType 0 is a standard
    // request, which the device stalls on, instead of a plausible SET_CUR that
    // silently retunes something else when the caller ignores the status.
    RtlZeroMemory(Request, sizeof(*Request));

    // Every field but wLength is one byte wide. Refusing an out-of-range value
    // beats masking it into the control that byte happens to name.
    if (UnitId == 0 || UnitId > 0xFF ||
        InterfaceNumber > 0xFF ||
        ControlSelector == 0 || ControlSelector > 0xFF ||
        Channel > 0xFF ||
        PayloadLength > EMU_CTRL_PAYLOAD_MAX) {
        return STATUS_INVALID_PARAMETER;
    }

    Request->Bytes[0] = RequestType;
    Request->Bytes[1] = EMU_CTRL_CS_CUR;
    Request->Bytes[2] = (UCHAR)Channel;                   // wValue, low byte
    Request->Bytes[3] = (UCHAR)ControlSelector;           // wValue, high byte
    Request->Bytes[4] = (UCHAR)InterfaceNumber;           // wIndex, low byte
    Request->Bytes[5] = (UCHAR)UnitId;                    // wIndex, high byte
    Request->Bytes[6] = (UCHAR)(PayloadLength & 0xFF);    // wLength, LE
    Request->Bytes[7] = (UCHAR)((PayloadLength >> 8) & 0xFF);
    Request->PayloadLength = PayloadLength;
    return STATUS_SUCCESS;
}

NTSTATUS
EmuCtrlBuildRequest(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ BOOLEAN ToDevice,
    _In_ ULONG UnitId,
    _In_ ULONG InterfaceNumber,
    _In_ ULONG ControlSelector,
    _In_ ULONG Channel,
    _In_ ULONG PayloadLength
    )
{
    return EmuCtrlBuildInto(Request,
                            ToDevice ? EMU_CTRL_REQUEST_TYPE_OUT
                                     : EMU_CTRL_REQUEST_TYPE_IN,
                            UnitId, InterfaceNumber, ControlSelector, Channel,
                            PayloadLength);
}

BOOLEAN
EmuCtrlRateToIndex(
    _In_ ULONG RateHz,
    _Out_ PUCHAR Index
    )
{
    ULONG i;

    if (Index == NULL) {
        return FALSE;
    }
    for (i = 0; i < EMU_XU_RATE_COUNT; i++) {
        if (g_EmuXuRates[i] == RateHz) {
            *Index = (UCHAR)i;
            return TRUE;
        }
    }
    return FALSE;
}

ULONG
EmuCtrlRateFromIndex(
    _In_ UCHAR Index
    )
{
    return (Index < EMU_XU_RATE_COUNT) ? g_EmuXuRates[Index] : 0;
}

VOID
EmuCtrlWriteDigitalRate(
    _Out_writes_bytes_(4) PUCHAR Payload,
    _In_ ULONG RateHz
    )
{
    Payload[0] = (UCHAR)(RateHz >> 24);
    Payload[1] = (UCHAR)(RateHz >> 16);
    Payload[2] = (UCHAR)(RateHz >> 8);
    Payload[3] = (UCHAR)RateHz;
}

ULONG
EmuCtrlReadDigitalRate(
    _In_reads_bytes_(4) const UCHAR* Payload
    )
{
    return ((ULONG)Payload[0] << 24) |
           ((ULONG)Payload[1] << 16) |
           ((ULONG)Payload[2] << 8) |
            (ULONG)Payload[3];
}

VOID
EmuCtrlWriteVolume(
    _Out_writes_bytes_(2) PUCHAR Payload,
    _In_ SHORT Two56thsOfDb
    )
{
    USHORT raw = (USHORT)Two56thsOfDb;

    Payload[0] = (UCHAR)(raw & 0xFF);
    Payload[1] = (UCHAR)(raw >> 8);
}

SHORT
EmuCtrlReadVolume(
    _In_reads_bytes_(2) const UCHAR* Payload
    )
{
    return (SHORT)((USHORT)Payload[0] | ((USHORT)Payload[1] << 8));
}

NTSTATUS
EmuCtrlBuildClockRateSet(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG UnitId,
    _In_ ULONG AcInterface,
    _In_ ULONG RateHz
    )
{
    UCHAR index;
    NTSTATUS status;

    status = EmuCtrlBuildInto(Request, EMU_CTRL_REQUEST_TYPE_OUT, UnitId,
                              AcInterface, EMU_XU_CS_CLOCK_RATE_SELECT, 0, 1);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (!EmuCtrlRateToIndex(RateHz, &index)) {
        // The device has no index for this rate. Zero the request: index 0 would
        // be a valid SET_CUR to 44100, and a caller that dropped the status must
        // not be able to retune the clock by accident.
        RtlZeroMemory(Request, sizeof(*Request));
        return STATUS_NOT_SUPPORTED;
    }
    Request->Payload[0] = index;
    return STATUS_SUCCESS;
}

static NTSTATUS
EmuCtrlBuildByteSet(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG UnitId,
    _In_ ULONG AcInterface,
    _In_ ULONG ControlSelector,
    _In_ ULONG Value,
    _In_ ULONG ValueMax
    )
{
    NTSTATUS status;

    status = EmuCtrlBuildInto(Request, EMU_CTRL_REQUEST_TYPE_OUT, UnitId,
                              AcInterface, ControlSelector, 0, 1);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (Value > ValueMax) {
        // Same shape as every other failure: a rejected build cannot be sent as a
        // valid request for control 0.
        RtlZeroMemory(Request, sizeof(*Request));
        return STATUS_INVALID_PARAMETER;
    }
    Request->Payload[0] = (UCHAR)Value;
    return STATUS_SUCCESS;
}

NTSTATUS
EmuCtrlBuildClockSourceSet(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG UnitId,
    _In_ ULONG AcInterface,
    _In_ ULONG Source
    )
{
    // 0 = internal, 1 = S/PDIF: nothing else exists on CA0189.
    return EmuCtrlBuildByteSet(Request, UnitId, AcInterface,
                               EMU_XU_CS_CLOCK_SOURCE, Source, 1);
}

NTSTATUS
EmuCtrlBuildSoftLimitSet(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG UnitId,
    _In_ ULONG AcInterface,
    _In_ BOOLEAN Enabled
    )
{
    return EmuCtrlBuildByteSet(Request, UnitId, AcInterface,
                               EMU_XU_CS_OPTION_SOFT_LIMIT,
                               Enabled ? 1 : 0, 1);
}

NTSTATUS
EmuCtrlBuildSpdifFormatSet(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG UnitId,
    _In_ ULONG AcInterface,
    _In_ ULONG Format
    )
{
    // 0 none, 1 S/PDIF, 2 AES.
    return EmuCtrlBuildByteSet(Request, UnitId, AcInterface,
                               EMU_XU_CS_DIGITAL_FORMAT, Format, 2);
}

NTSTATUS
EmuCtrlBuildMuteOff(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG FeatureUnitId,
    _In_ ULONG AcInterface
    )
{
    return EmuCtrlBuildByteSet(Request, FeatureUnitId, AcInterface,
                               EMU_FEATURE_CS_MUTE, 0, 1);
}

NTSTATUS
EmuCtrlBuildVolumeSet(
    _Out_ PEMU_CTRL_REQUEST Request,
    _In_ ULONG FeatureUnitId,
    _In_ ULONG AcInterface,
    _In_ ULONG Channel,
    _In_ SHORT Volume
    )
{
    NTSTATUS status;

    status = EmuCtrlBuildInto(Request, EMU_CTRL_REQUEST_TYPE_OUT, FeatureUnitId,
                              AcInterface, EMU_FEATURE_CS_VOLUME, Channel, 2);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    Request->Payload[0] = (UCHAR)(Volume & 0xFF);
    Request->Payload[1] = (UCHAR)((Volume >> 8) & 0xFF);
    return STATUS_SUCCESS;
}
