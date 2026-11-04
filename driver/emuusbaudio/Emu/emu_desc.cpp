// EmuUSBAudio — UAC1 configuration-descriptor parser implementation.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Two passes over one bounded buffer:
//   pass A collects the facts the later decisions depend on (interface roles,
//          AudioControl terminals, Extension/Feature Units, AC status endpoint);
//   pass B groups AudioStreaming descriptors per alternate setting and resolves
//          each stream direction through its terminal link.
// Terminals must be known before any stream direction can be trusted, which is
// why the block is walked twice instead of once in descriptor order.
#include "emu_desc.h"

// Offsets inside the fixed-size descriptors.
#define EMU_IFACE_NUMBER_OFF     2
#define EMU_IFACE_ALT_OFF        3
#define EMU_IFACE_NUMEP_OFF      4
#define EMU_IFACE_CLASS_OFF      5
#define EMU_IFACE_SUBCLASS_OFF   6
#define EMU_IFACE_PROTOCOL_OFF   7
#define EMU_IFACE_LENGTH         9

#define EMU_EP_ADDRESS_OFF       2
#define EMU_EP_ATTRIBUTES_OFF    3
#define EMU_EP_MAXPACKET_OFF     4
#define EMU_EP_INTERVAL_OFF      6
#define EMU_EP_MIN_LENGTH        7

#define EMU_CS_SUBTYPE_OFF        2
#define EMU_TERM_ID_OFF           3
#define EMU_FEATURE_UNIT_OFF      3
#define EMU_FEATURE_SOURCE_OFF    4
#define EMU_FEATURE_CTRLSIZE_OFF  5
#define EMU_FEATURE_CONTROLS_OFF  6
#define EMU_XU_UNIT_OFF           3
#define EMU_XU_NR_SOURCES_OFF     4
#define EMU_XU_EMU_CODE_OFF       4
#define EMU_AS_TERMINAL_LINK_OFF  3
#define EMU_AS_FORMATTAG_OFF      5
#define EMU_FT_FORMATTYPE_OFF     3
#define EMU_FT_CHANNELS_OFF       4
#define EMU_FT_SUBFRAME_OFF       5
#define EMU_FT_BITRES_OFF         6
#define EMU_FT_SAMFREQTYPE_OFF    7
#define EMU_FT_RATES_OFF          8
#define EMU_FT_MIN_LENGTH         8
#define EMU_FT_CONTINUOUS_LENGTH 14

#define EMU_EP_XFER_INTERRUPT   0x03
#define EMU_EP_DIRECTION_IN     0x80

typedef struct _EMU_PARSE_CTX {
    const UCHAR* Descriptor;
    ULONG Region;
    PEMU_DEVICE_CAPS Caps;
    ULONG TerminalId[EMU_MAX_TERMINALS];
    BOOLEAN TerminalIsInput[EMU_MAX_TERMINALS];
    ULONG TerminalCount;
} EMU_PARSE_CTX, *PEMU_PARSE_CTX;

// One alternate setting, accumulated until the next interface descriptor or the
// end of the block commits it.
typedef struct _EMU_ALT_STATE {
    BOOLEAN Active;
    BOOLEAN GeneralSeen;
    BOOLEAN FormatSeen;
    BOOLEAN DataSeen;
    BOOLEAN FeedbackSeen;
    BOOLEAN NonPcm;
    ULONG   InterfaceNumber;
    ULONG   AltSetting;
    ULONG   TerminalId;
    ULONG   FormatTag;
    ULONG   Channels;
    ULONG   SubFrameSize;
    ULONG   BitResolution;
    ULONG   RateFlags;
    ULONG   RateCount;
    ULONG   Rates[EMU_MAX_RATES_PER_ALT];
    ULONG   ContinuousMinHz;
    ULONG   ContinuousMaxHz;
    ULONG   DataEndpoint;
    ULONG   DataMaxPacket;
    ULONG   DataAttributes;
    ULONG   DataUsageType;
    ULONG   DataInterval;
    ULONG   FeedbackEndpoint;
    ULONG   FeedbackMaxPacket;
    ULONG   FeedbackAttributes;
} EMU_ALT_STATE, *PEMU_ALT_STATE;

static BOOLEAN
EmuRangeOk(_In_ ULONG Offset, _In_ ULONG Need, _In_ ULONG Limit)
{
    return Need <= Limit && Offset <= Limit - Need;
}

static USHORT
EmuReadU16(_In_reads_bytes_(Offset + 2) const UCHAR* D, _In_ ULONG Offset)
{
    return (USHORT)((ULONG)D[Offset] | ((ULONG)D[Offset + 1] << 8));
}

static ULONG
EmuReadU24(_In_reads_bytes_(Offset + 3) const UCHAR* D, _In_ ULONG Offset)
{
    return (ULONG)D[Offset] | ((ULONG)D[Offset + 1] << 8) | ((ULONG)D[Offset + 2] << 16);
}

static BOOLEAN
EmuIsXuCode(_In_ ULONG Code)
{
    return Code >= EMU_XU_CLOCK_RATE && Code <= EMU_XU_METERING;
}

// Both passes walk the block with this step. bLength must cover at least its own
// two header bytes and must not run past the block; a block that does not end on
// a descriptor boundary is malformed, so the leftover 1 byte is an error too.
static NTSTATUS
EmuNextDescriptor(
    _In_reads_bytes_(Region) const UCHAR* D,
    _In_ ULONG Region,
    _Inout_ PULONG Offset,
    _Out_ PULONG Length
    )
{
    ULONG off = *Offset;

    if (off + 2 > Region) {
        return (off == Region) ? STATUS_NO_MORE_FILES : STATUS_DATA_ERROR;
    }
    ULONG len = D[off];
    if (len < 2 || !EmuRangeOk(off, len, Region)) {
        return STATUS_DATA_ERROR;
    }
    *Length = len;
    *Offset = off + len;
    return STATUS_SUCCESS;
}

static _Ret_maybenull_ PEMU_AUDIO_INTERFACE
EmuInterfaceRecord(_In_ PEMU_PARSE_CTX Ctx, _In_ ULONG InterfaceNumber)
{
    PEMU_DEVICE_CAPS caps = Ctx->Caps;

    for (ULONG i = 0; i < caps->AudioInterfaceCount; i++) {
        if (caps->AudioInterfaces[i].InterfaceNumber == InterfaceNumber) {
            return &caps->AudioInterfaces[i];
        }
    }
    return NULL;
}

static NTSTATUS
EmuAddTerminal(_In_ PEMU_PARSE_CTX Ctx, _In_ ULONG TerminalId, _In_ BOOLEAN IsInput)
{
    if (TerminalId == 0) {
        return STATUS_DATA_ERROR;
    }
    for (ULONG i = 0; i < Ctx->TerminalCount; i++) {
        if (Ctx->TerminalId[i] == TerminalId) {
            if (Ctx->TerminalIsInput[i] != IsInput) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            return STATUS_SUCCESS;
        }
    }
    if (Ctx->TerminalCount >= EMU_MAX_TERMINALS) {
        return STATUS_BUFFER_OVERFLOW;
    }
    Ctx->TerminalId[Ctx->TerminalCount] = TerminalId;
    Ctx->TerminalIsInput[Ctx->TerminalCount] = IsInput;
    Ctx->TerminalCount++;
    return STATUS_SUCCESS;
}

static NTSTATUS
EmuResolveTerminal(_In_ PEMU_PARSE_CTX Ctx, _In_ ULONG TerminalId, _Out_ PULONG Direction)
{
    for (ULONG i = 0; i < Ctx->TerminalCount; i++) {
        if (Ctx->TerminalId[i] == TerminalId) {
            // bTerminalLink names the terminal where the stream crosses the
            // unit graph, and the direction follows from which side of the
            // graph that terminal is on: a host-to-device stream enters the
            // device through a USB-streaming INPUT terminal, a device-to-host
            // stream leaves through an OUTPUT terminal. The live 0404 spells
            // this out plainly — the render interface links terminal 0x01 (an
            // input terminal), the capture interface terminal 0x0A (an output
            // terminal).
            *Direction = Ctx->TerminalIsInput[i] ? EMU_STREAM_RENDER
                                                 : EMU_STREAM_CAPTURE;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

// EXTENSION_UNIT, Table A-5 subtype 8. Class layout puts bNrSources and the
// source list between the unit ID and wExtensionCode; CA0189 puts
// wExtensionCode directly after the unit ID. The E-MU form is tried first and
// accepted only when the two bytes spell a known code — in the class form those
// bytes are bNrSources plus the first source id, small numbers that cannot
// reach 0xE3xx.
static NTSTATUS
EmuParseExtensionUnit(_In_ PEMU_PARSE_CTX Ctx, _In_ ULONG Start, _In_ ULONG Length)
{
    const UCHAR* d = Ctx->Descriptor;
    PEMU_DEVICE_CAPS caps = Ctx->Caps;
    ULONG unitId = d[Start + EMU_XU_UNIT_OFF];
    ULONG code;
    ULONG layout;
    ULONG controlSize = 0;

    if (Length < 6) {
        return STATUS_DATA_ERROR;
    }
    code = EmuReadU16(d, Start + EMU_XU_EMU_CODE_OFF);
    if (EmuIsXuCode(code)) {
        layout = EMU_XU_LAYOUT_EMU;
    } else {
        ULONG sources = d[Start + EMU_XU_NR_SOURCES_OFF];
        ULONG codeOffset = Start + 5 + sources * 5;
        if (sources == 0 || sources > 4 || !EmuRangeOk(codeOffset, 2, Start + Length)) {
            return STATUS_DATA_ERROR;
        }
        code = EmuReadU16(d, codeOffset);
        layout = EMU_XU_LAYOUT_SPEC;
        if (EmuRangeOk(codeOffset + 2, 1, Start + Length)) {
            controlSize = d[codeOffset + 2];
        }
    }
    for (ULONG i = 0; i < caps->XuUnitCount; i++) {
        if (caps->XuUnits[i].UnitID == unitId) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    if (caps->XuUnitCount >= EMU_MAX_XU_UNITS) {
        return STATUS_BUFFER_OVERFLOW;
    }
    PEMU_XU_UNIT xu = &caps->XuUnits[caps->XuUnitCount++];
    xu->UnitID = unitId;
    xu->ExtensionCode = code;
    xu->InterfaceNumber = caps->AcInterfaceNumber;
    xu->Layout = layout;
    xu->ControlSize = controlSize;
    caps->ParsedFlags |= EMU_CAPS_HAS_XU;
    return STATUS_SUCCESS;
}

static NTSTATUS
EmuParseFeatureUnit(_In_ PEMU_PARSE_CTX Ctx, _In_ ULONG Start, _In_ ULONG Length)
{
    const UCHAR* d = Ctx->Descriptor;
    PEMU_DEVICE_CAPS caps = Ctx->Caps;

    // [3] bUnitID, [4] bSourceID, [5] bControlSize, bmaControls[channels + 1],
    // last byte iFeature: the control block must divide by bControlSize exactly.
    if (Length < 7) {
        return STATUS_DATA_ERROR;
    }
    ULONG controlSize = d[Start + EMU_FEATURE_CTRLSIZE_OFF];
    if (controlSize == 0 || controlSize > 4) {
        return STATUS_DATA_ERROR;
    }
    ULONG controlBytes = Length - 6 - 1;
    if (controlBytes == 0 || controlBytes % controlSize != 0) {
        return STATUS_DATA_ERROR;
    }
    ULONG entries = controlBytes / controlSize;
    if (caps->FeatureUnitCount >= EMU_MAX_FEATURE_UNITS) {
        return STATUS_BUFFER_OVERFLOW;
    }
    PEMU_FEATURE_UNIT fu = &caps->FeatureUnits[caps->FeatureUnitCount++];
    fu->UnitID = d[Start + EMU_FEATURE_UNIT_OFF];
    fu->SourceID = d[Start + EMU_FEATURE_SOURCE_OFF];
    fu->InterfaceNumber = caps->AcInterfaceNumber;
    fu->ControlSize = controlSize;
    fu->ChannelCount = entries - 1;
    fu->MasterControls = d[Start + EMU_FEATURE_CONTROLS_OFF];
    return STATUS_SUCCESS;
}

// Pass A: interface roles, terminals, AC units, AC status endpoint.
static NTSTATUS
EmuPassCollectAcFacts(_In_ PEMU_PARSE_CTX Ctx)
{
    const UCHAR* d = Ctx->Descriptor;
    PEMU_DEVICE_CAPS caps = Ctx->Caps;
    ULONG off = Ctx->Descriptor[0];
    ULONG len;
    ULONG currentNumber = EMU_NO_VALUE;
    ULONG currentRole = EMU_ROLE_UNKNOWN;
    PEMU_AUDIO_INTERFACE current = NULL;

    for (;;) {
        NTSTATUS status = EmuNextDescriptor(d, Ctx->Region, &off, &len);
        if (status == STATUS_NO_MORE_FILES) {
            break;
        }
        if (!NT_SUCCESS(status)) {
            return status;
        }
        ULONG start = off - len;
        ULONG type = d[start + 1];

        if (type == EMU_DT_INTERFACE) {
            if (len < EMU_IFACE_LENGTH) {
                return STATUS_DATA_ERROR;
            }
            ULONG number = d[start + EMU_IFACE_NUMBER_OFF];
            ULONG declaredClass = d[start + EMU_IFACE_CLASS_OFF];
            ULONG declaredSubclass = d[start + EMU_IFACE_SUBCLASS_OFF];
            ULONG declaredProtocol = d[start + EMU_IFACE_PROTOCOL_OFF];

            if (declaredClass != EMU_INTERFACE_CLASS_AUDIO &&
                declaredClass != EMU_INTERFACE_CLASS_VENDOR) {
                // Not ours (HID, vendor data, ...): forget the current context so
                // its class-specific descriptors cannot be read as audio.
                currentNumber = EMU_NO_VALUE;
                currentRole = EMU_ROLE_UNKNOWN;
                current = NULL;
                continue;
            }
            if (current == NULL || currentNumber != number) {
                current = EmuInterfaceRecord(Ctx, number);
                if (current == NULL) {
                    if (caps->AudioInterfaceCount >= EMU_MAX_AUDIO_INTERFACES) {
                        return STATUS_BUFFER_OVERFLOW;
                    }
                    current = &caps->AudioInterfaces[caps->AudioInterfaceCount++];
                    current->InterfaceNumber = number;
                    current->Role = EMU_ROLE_UNKNOWN;
                    current->StatusEndpoint = EMU_NO_VALUE;
                    current->DeclaredClass = declaredClass;
                    current->DeclaredSubclass = declaredSubclass;
                    current->DeclaredProtocol = declaredProtocol;
                } else {
                    // Alternate settings of one interface must agree, or the
                    // role we picked for it is not the role of this one.
                    if (current->DeclaredClass != declaredClass ||
                        current->DeclaredSubclass != declaredSubclass ||
                        current->DeclaredProtocol != declaredProtocol) {
                        return STATUS_DEVICE_CONFIGURATION_ERROR;
                    }
                }
            }
            current->DeclaredNumEndpoints = d[start + EMU_IFACE_NUMEP_OFF];
            currentNumber = number;
            currentRole = current->Role;
            if (currentRole == EMU_ROLE_UNKNOWN) {
                // CA0189 hides the class byte (0xFF) but leaves bInterfaceSubClass
                // standard — the v1 engine keys on the same pair
                // (src/emu_usb.cpp:220). Subtype numbering of the class-specific
                // descriptors differs between AC and AS, so this byte is what
                // makes the interpretation unambiguous. A device that hid it too
                // would be skipped here: no binding, not a wrong parse.
                switch (declaredSubclass) {
                case EMU_AUDIO_SUBCLASS_CONTROL:
                    currentRole = current->Role = EMU_ROLE_AC;
                    break;
                case EMU_AUDIO_SUBCLASS_STREAMING:
                    currentRole = current->Role = EMU_ROLE_AS;
                    break;
                case EMU_AUDIO_SUBCLASS_MIDI_STREAMING:
                    currentRole = current->Role = EMU_ROLE_MIDI;
                    caps->ParsedFlags |= EMU_CAPS_MIDI_INTERFACE;
                    break;
                default:
                    break;
                }
            }
            if (currentRole != EMU_ROLE_UNKNOWN &&
                declaredClass == EMU_INTERFACE_CLASS_VENDOR) {
                // M4's binding and the INF need to know that the audio class byte
                // is disguised; a standard UAC1 device never sets this.
                caps->ParsedFlags |= EMU_CAPS_VENDOR_CLASS;
            }
            continue;
        }

        if (type == EMU_DT_CS_INTERFACE && current != NULL) {
            if (len < 3) {
                return STATUS_DATA_ERROR;
            }
            ULONG subtype = d[start + EMU_CS_SUBTYPE_OFF];

            if (currentRole == EMU_ROLE_AC) {
                switch (subtype) {
                case EMU_SUBTYPE_AC_HEADER:
                    if (len < 8) {
                        return STATUS_DATA_ERROR;
                    }
                    if (caps->AcInterfaceNumber != EMU_NO_VALUE &&
                        caps->AcInterfaceNumber != currentNumber) {
                        return STATUS_DEVICE_CONFIGURATION_ERROR;
                    }
                    caps->AcInterfaceNumber = currentNumber;
                    caps->ParsedFlags |= EMU_CAPS_AC_FOUND;
                    break;
                case EMU_SUBTYPE_AC_INPUT_TERMINAL:
                case EMU_SUBTYPE_AC_OUTPUT_TERMINAL:
                    if (len < 6) {
                        return STATUS_DATA_ERROR;
                    }
                    status = EmuAddTerminal(Ctx,
                                            d[start + EMU_TERM_ID_OFF],
                                            subtype == EMU_SUBTYPE_AC_INPUT_TERMINAL);
                    if (!NT_SUCCESS(status)) {
                        return status;
                    }
                    break;
                case EMU_SUBTYPE_AC_FEATURE_UNIT:
                    status = EmuParseFeatureUnit(Ctx, start, len);
                    if (!NT_SUCCESS(status)) {
                        return status;
                    }
                    break;
                case EMU_SUBTYPE_AC_EXTENSION_UNIT:
                    status = EmuParseExtensionUnit(Ctx, start, len);
                    if (!NT_SUCCESS(status)) {
                        return status;
                    }
                    break;
                default:
                    // MIXER_UNIT, SELECTOR_UNIT, PROCESSING_UNIT: not needed by
                    // M2, skipped by length.
                    break;
                }
            }
            // AudioStreaming class-specific descriptors are pass B's business.
            continue;
        }

        if (type == EMU_DT_ENDPOINT && currentRole == EMU_ROLE_AC && current != NULL) {
            if (len < EMU_EP_MIN_LENGTH) {
                return STATUS_DATA_ERROR;
            }
            if (((d[start + EMU_EP_ATTRIBUTES_OFF] & EMU_EP_XFER_MASK) ==
                 EMU_EP_XFER_INTERRUPT) &&
                (current->StatusEndpoint == EMU_NO_VALUE)) {
                current->StatusEndpoint = d[start + EMU_EP_ADDRESS_OFF];
                current->StatusMaxPacket =
                    EmuReadU16(d, start + EMU_EP_MAXPACKET_OFF) & 0x07FF;
                current->StatusInterval = d[start + EMU_EP_INTERVAL_OFF];
                if (caps->StatusEndpoint == EMU_NO_VALUE) {
                    caps->StatusEndpoint = current->StatusEndpoint;
                    caps->ParsedFlags |= EMU_CAPS_STATUS_ENDPOINT;
                }
            }
        }
    }

    if (caps->AcInterfaceNumber == EMU_NO_VALUE) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
EmuParseStreamingGeneral(_Inout_ PEMU_ALT_STATE Alt, _In_reads_bytes_(Start + Length) const UCHAR* d,
                         _In_ ULONG Start, _In_ ULONG Length)
{
    if (Length < 7) {
        return STATUS_DATA_ERROR;
    }
    if (Alt->GeneralSeen) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Alt->GeneralSeen = TRUE;
    // bTerminalLink is a byte; a 16-bit read here would swallow bDelay and
    // resolve to a terminal no descriptor names.
    Alt->TerminalId = d[Start + EMU_AS_TERMINAL_LINK_OFF];
    Alt->FormatTag = EmuReadU16(d, Start + EMU_AS_FORMATTAG_OFF);
    if (Alt->FormatTag != 1) {
        // Not PCM (vendor format): recorded for diagnostics, never streamed.
        Alt->NonPcm = TRUE;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
EmuParseFormatTypeI(_Inout_ PEMU_ALT_STATE Alt, _In_reads_bytes_(Start + Length) const UCHAR* d,
                    _In_ ULONG Start, _In_ ULONG Length)
{
    if (Length < EMU_FT_MIN_LENGTH) {
        return STATUS_DATA_ERROR;
    }
    if (d[Start + EMU_FT_FORMATTYPE_OFF] != 1) {
        // FORMAT_TYPE_II/III: PCM-shaped formats this driver does not stream.
        // Marked like a vendor format tag rather than refused — the live 0404
        // declares its 16-bit alternate settings as Type III, and failing the
        // whole device over alts nobody uses would trade a working parse for
        // nothing.
        Alt->NonPcm = TRUE;
        return STATUS_SUCCESS;
    }
    ULONG channels = d[Start + EMU_FT_CHANNELS_OFF];
    ULONG subFrame = d[Start + EMU_FT_SUBFRAME_OFF];
    ULONG bitRes = d[Start + EMU_FT_BITRES_OFF];
    if (Alt->FormatSeen) {
        // Several FORMAT_TYPE_I blocks in one alt may add rates, but they
        // cannot describe a different frame layout.
        if (channels != Alt->Channels || subFrame != Alt->SubFrameSize ||
            bitRes != Alt->BitResolution) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    } else {
        Alt->FormatSeen = TRUE;
        Alt->Channels = channels;
        Alt->SubFrameSize = subFrame;
        Alt->BitResolution = bitRes;
    }
    if (channels == 0 || subFrame == 0 || subFrame > 4 || bitRes == 0 ||
        bitRes > subFrame * 8) {
        return STATUS_DATA_ERROR;
    }

    ULONG samFreqType = d[Start + EMU_FT_SAMFREQTYPE_OFF];
    if (samFreqType == 0) {
        // Continuous: two 3-byte rates, operating minimum then maximum.
        if (Length < EMU_FT_CONTINUOUS_LENGTH) {
            return STATUS_DATA_ERROR;
        }
        Alt->RateFlags |= EMU_ALT_CONTINUOUS_RATES;
        Alt->ContinuousMinHz = EmuReadU24(d, Start + EMU_FT_RATES_OFF);
        Alt->ContinuousMaxHz = EmuReadU24(d, Start + EMU_FT_RATES_OFF + 3);
        if (Alt->ContinuousMinHz == 0 || Alt->ContinuousMaxHz < Alt->ContinuousMinHz) {
            return STATUS_DATA_ERROR;
        }
        return STATUS_SUCCESS;
    }

    if (!EmuRangeOk(Start + EMU_FT_RATES_OFF, samFreqType * 3, Start + Length)) {
        return STATUS_DATA_ERROR;
    }
    if (samFreqType > EMU_MAX_RATES_PER_ALT - Alt->RateCount) {
        return STATUS_BUFFER_OVERFLOW;
    }
    for (ULONG i = 0; i < samFreqType; i++) {
        ULONG rate = EmuReadU24(d, Start + EMU_FT_RATES_OFF + i * 3);
        if (rate == 0) {
            return STATUS_DATA_ERROR;
        }
        BOOLEAN duplicate = FALSE;
        for (ULONG j = 0; j < Alt->RateCount; j++) {
            if (Alt->Rates[j] == rate) {
                duplicate = TRUE;
                break;
            }
        }
        if (!duplicate) {
            Alt->Rates[Alt->RateCount++] = rate;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
EmuParseAltEndpoint(_Inout_ PEMU_ALT_STATE Alt, _In_reads_bytes_(Start + Length) const UCHAR* d,
                    _In_ ULONG Start, _In_ ULONG Length)
{
    if (Length < EMU_EP_MIN_LENGTH) {
        return STATUS_DATA_ERROR;
    }
    ULONG attributes = d[Start + EMU_EP_ATTRIBUTES_OFF];
    if ((attributes & EMU_EP_XFER_MASK) != EMU_EP_XFER_ISOCHRONOUS) {
        return STATUS_SUCCESS;      // bulk/interrupt endpoint inside an alt: not a stream
    }

    ULONG address = d[Start + EMU_EP_ADDRESS_OFF];
    ULONG maxPacket = EmuReadU16(d, Start + EMU_EP_MAXPACKET_OFF) & EMU_EP_ISO_MAX_PACKET;
    ULONG usage = (attributes >> EMU_EP_USAGE_SHIFT) & EMU_EP_USAGE_MASK;
    BOOLEAN isFeedback = (usage == EMU_EP_USAGE_FEEDBACK ||
                          usage == EMU_EP_USAGE_RESERVED_FB) ? TRUE : FALSE;

    if (maxPacket == 0) {
        return STATUS_DATA_ERROR;
    }
    if (isFeedback) {
        if (Alt->FeedbackSeen) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        Alt->FeedbackSeen = TRUE;
        Alt->FeedbackEndpoint = address;
        Alt->FeedbackMaxPacket = maxPacket;
        Alt->FeedbackAttributes = attributes;
        return STATUS_SUCCESS;
    }
    if (Alt->DataSeen) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Alt->DataSeen = TRUE;
    Alt->DataEndpoint = address;
    Alt->DataMaxPacket = maxPacket;
    Alt->DataAttributes = attributes;
    Alt->DataUsageType = usage;
    Alt->DataInterval = d[Start + EMU_EP_INTERVAL_OFF];
    return STATUS_SUCCESS;
}

// Turn one accumulated alternate setting into a caps entry, or drop it.
//
// Alternate setting 0 is the UAC1 zero-bandwidth setting: M2 selects it to stop
// a stream, it is not a stream itself, so it is never recorded. Every alt above
// 0 has to be complete — AS_GENERAL plus a PCM format plus one data endpoint —
// because a half-described alt would let M3 build a pipe that does not exist.
static NTSTATUS
EmuCommitAlt(_In_ PEMU_PARSE_CTX Ctx, _Inout_ PEMU_ALT_STATE Alt)
{
    PEMU_DEVICE_CAPS caps = Ctx->Caps;
    NTSTATUS status = STATUS_SUCCESS;

    if (!Alt->Active) {
        return STATUS_SUCCESS;
    }
    if (Alt->AltSetting == 0) {
        Alt->Active = FALSE;
        return STATUS_SUCCESS;
    }
    if (Alt->NonPcm) {
        caps->ParsedFlags |= EMU_CAPS_NON_PCM_FORMAT;
        Alt->Active = FALSE;
        return STATUS_SUCCESS;
    }
    if (!Alt->GeneralSeen || !Alt->FormatSeen || !Alt->DataSeen) {
        Alt->Active = FALSE;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (Alt->RateCount == 0 && !(Alt->RateFlags & EMU_ALT_CONTINUOUS_RATES)) {
        Alt->Active = FALSE;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    for (ULONG i = 0; i < caps->StreamAltCount; i++) {
        if (caps->StreamAlts[i].InterfaceNumber == Alt->InterfaceNumber &&
            caps->StreamAlts[i].AltSetting == Alt->AltSetting) {
            Alt->Active = FALSE;
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    if (caps->StreamAltCount >= EMU_MAX_STREAM_ALTS) {
        Alt->Active = FALSE;
        return STATUS_BUFFER_OVERFLOW;
    }

    ULONG direction;
    status = EmuResolveTerminal(Ctx, Alt->TerminalId, &direction);
    if (!NT_SUCCESS(status)) {
        Alt->Active = FALSE;
        return status;
    }
    BOOLEAN dataIsIn = (Alt->DataEndpoint & EMU_EP_DIRECTION_IN) != 0;
    if ((direction == EMU_STREAM_CAPTURE) != dataIsIn) {
        Alt->Active = FALSE;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    PEMU_STREAM_ALT entry = &caps->StreamAlts[caps->StreamAltCount++];
    entry->InterfaceNumber = Alt->InterfaceNumber;
    entry->AltSetting = Alt->AltSetting;
    entry->Direction = direction;
    entry->TerminalId = Alt->TerminalId;
    entry->FormatTag = Alt->FormatTag;
    entry->Channels = Alt->Channels;
    entry->SubFrameSize = Alt->SubFrameSize;
    entry->BitResolution = Alt->BitResolution;
    entry->RateFlags = Alt->RateFlags;
    entry->RateCount = Alt->RateCount;
    for (ULONG i = 0; i < Alt->RateCount; i++) {
        entry->Rates[i] = Alt->Rates[i];
    }
    entry->ContinuousMinHz = Alt->ContinuousMinHz;
    entry->ContinuousMaxHz = Alt->ContinuousMaxHz;
    entry->DataEndpoint = Alt->DataEndpoint;
    entry->DataMaxPacket = Alt->DataMaxPacket;
    entry->DataAttributes = Alt->DataAttributes;
    entry->DataUsageType = Alt->DataUsageType;
    entry->DataInterval = Alt->DataInterval;
    entry->FeedbackEndpoint = Alt->FeedbackSeen ? Alt->FeedbackEndpoint : EMU_NO_VALUE;
    entry->FeedbackMaxPacket = Alt->FeedbackMaxPacket;
    entry->FeedbackAttributes = Alt->FeedbackAttributes;

    caps->ParsedFlags |= (direction == EMU_STREAM_CAPTURE) ? EMU_CAPS_CAPTURE
                                                           : EMU_CAPS_RENDER;
    if (Alt->Channels > caps->MaxChannels) {
        caps->MaxChannels = Alt->Channels;
    }
    for (ULONG i = 0; i < Alt->RateCount; i++) {
        if (Alt->Rates[i] < caps->MinRateHz) {
            caps->MinRateHz = Alt->Rates[i];
        }
        if (Alt->Rates[i] > caps->MaxRateHz) {
            caps->MaxRateHz = Alt->Rates[i];
        }
    }
    if (Alt->RateFlags & EMU_ALT_CONTINUOUS_RATES) {
        if (Alt->ContinuousMinHz < caps->MinRateHz) {
            caps->MinRateHz = Alt->ContinuousMinHz;
        }
        if (Alt->ContinuousMaxHz > caps->MaxRateHz) {
            caps->MaxRateHz = Alt->ContinuousMaxHz;
        }
    }
    Alt->Active = FALSE;
    return STATUS_SUCCESS;
}

static VOID
EmuBeginAlt(_Inout_ PEMU_ALT_STATE Alt, _In_ ULONG InterfaceNumber, _In_ ULONG AltSetting)
{
    RtlZeroMemory(Alt, sizeof(*Alt));
    Alt->Active = TRUE;
    Alt->InterfaceNumber = InterfaceNumber;
    Alt->AltSetting = AltSetting;
}

// Pass B: AudioStreaming interfaces, alternate settings and their endpoints.
static NTSTATUS
EmuPassCollectStreams(_In_ PEMU_PARSE_CTX Ctx)
{
    const UCHAR* d = Ctx->Descriptor;
    EMU_ALT_STATE alt;
    ULONG off = Ctx->Descriptor[0];
    ULONG len;
    PEMU_AUDIO_INTERFACE current = NULL;
    NTSTATUS status;

    RtlZeroMemory(&alt, sizeof(alt));

    for (;;) {
        status = EmuNextDescriptor(d, Ctx->Region, &off, &len);
        if (status == STATUS_NO_MORE_FILES) {
            break;
        }
        if (!NT_SUCCESS(status)) {
            return status;
        }
        ULONG start = off - len;
        ULONG type = d[start + 1];

        if (type == EMU_DT_INTERFACE) {
            status = EmuCommitAlt(Ctx, &alt);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            ULONG number = d[start + EMU_IFACE_NUMBER_OFF];
            current = EmuInterfaceRecord(Ctx, number);
            if (current != NULL && current->Role == EMU_ROLE_AS) {
                EmuBeginAlt(&alt, number, d[start + EMU_IFACE_ALT_OFF]);
            }
            continue;
        }
        if (!alt.Active) {
            continue;
        }
        if (type == EMU_DT_CS_INTERFACE) {
            if (len < 3) {
                return STATUS_DATA_ERROR;
            }
            ULONG subtype = d[start + EMU_CS_SUBTYPE_OFF];
            if (subtype == EMU_SUBTYPE_AS_GENERAL) {
                status = EmuParseStreamingGeneral(&alt, d, start, len);
            } else if (subtype == EMU_SUBTYPE_AS_FORMAT_TYPE_I) {
                status = EmuParseFormatTypeI(&alt, d, start, len);
            } else if (subtype == EMU_SUBTYPE_AS_FORMAT_TYPE_II ||
                       subtype == EMU_SUBTYPE_AS_FORMAT_TYPE_III) {
                alt.NonPcm = TRUE;
                status = STATUS_SUCCESS;
            } else {
                status = STATUS_SUCCESS;   // CS_AC_* / vendor subtypes: ignored
            }
            if (!NT_SUCCESS(status)) {
                return status;
            }
            continue;
        }
        if (type == EMU_DT_ENDPOINT || type == EMU_DT_CS_ENDPOINT) {
            if (type == EMU_DT_ENDPOINT) {
                status = EmuParseAltEndpoint(&alt, d, start, len);
                if (!NT_SUCCESS(status)) {
                    return status;
                }
            }
            continue;
        }
    }
    return EmuCommitAlt(Ctx, &alt);
}

static VOID
EmuInitCaps(_Out_ PEMU_DEVICE_CAPS Caps)
{
    RtlZeroMemory(Caps, sizeof(*Caps));
    Caps->AcInterfaceNumber = EMU_NO_VALUE;
    Caps->StatusEndpoint = EMU_NO_VALUE;
    Caps->MinRateHz = 0xFFFFFFFFu;
}

NTSTATUS
EmuParseUsbConfigDescriptor(
    _In_reads_bytes_(BufferLength) const UCHAR* Descriptor,
    _In_ ULONG BufferLength,
    _Out_ PEMU_DEVICE_CAPS Caps
    )
{
    EMU_PARSE_CTX ctx;
    NTSTATUS status;

    if (Descriptor == NULL || Caps == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    EmuInitCaps(Caps);

    // A configuration descriptor block always starts with its own header, and
    // wTotalLength must equal what the caller handed over: M2 reads the header
    // first, then exactly that many bytes. A short block is not a partial
    // success — half a descriptor table is worse than none.
    if (BufferLength < EMU_CONFIG_HEADER_MIN_LENGTH ||
        Descriptor[0] < EMU_CONFIG_HEADER_MIN_LENGTH ||
        Descriptor[0] > BufferLength ||
        Descriptor[1] != EMU_DT_CONFIGURATION) {
        return STATUS_DATA_ERROR;
    }
    USHORT declared = EmuReadU16(Descriptor, 2);
    if (declared != BufferLength) {
        return STATUS_DATA_ERROR;
    }

    Caps->ConfigurationValue = Descriptor[5];
    Caps->DeclaredTotalLength = declared;
    Caps->BufferLength = BufferLength;

    RtlZeroMemory(&ctx, sizeof(ctx));
    ctx.Descriptor = Descriptor;
    ctx.Region = BufferLength;
    ctx.Caps = Caps;

    status = EmuPassCollectAcFacts(&ctx);
    if (!NT_SUCCESS(status)) {
        goto Fail;
    }
    status = EmuPassCollectStreams(&ctx);
    if (!NT_SUCCESS(status)) {
        goto Fail;
    }
    if (Caps->StreamAltCount == 0) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Fail;
    }
    Caps->MinRateHz = (Caps->MinRateHz == 0xFFFFFFFFu) ? 0 : Caps->MinRateHz;
    return STATUS_SUCCESS;

Fail:
    // The caller can keep one Caps across start/stop cycles: a failed parse has
    // to leave it empty, not half-filled and therefore usable-looking.
    EmuInitCaps(Caps);
    return status;
}

ULONG
EmuCapsXuUnitId(_In_ const EMU_DEVICE_CAPS* Caps, _In_ ULONG ExtensionCode)
{
    for (ULONG i = 0; i < Caps->XuUnitCount; i++) {
        if (Caps->XuUnits[i].ExtensionCode == ExtensionCode) {
            return Caps->XuUnits[i].UnitID;
        }
    }
    return EMU_NO_VALUE;
}

ULONG
EmuCapsAltCount(_In_ const EMU_DEVICE_CAPS* Caps, _In_ ULONG Direction)
{
    ULONG count = 0;

    for (ULONG i = 0; i < Caps->StreamAltCount; i++) {
        if (Caps->StreamAlts[i].Direction == Direction) {
            count++;
        }
    }
    return count;
}

BOOLEAN
EmuAltSupportsRate(_In_ const EMU_STREAM_ALT* Alt, _In_ ULONG RateHz)
{
    if (Alt->RateFlags & EMU_ALT_CONTINUOUS_RATES) {
        return (BOOLEAN)(RateHz >= Alt->ContinuousMinHz &&
                         RateHz <= Alt->ContinuousMaxHz);
    }
    for (ULONG i = 0; i < Alt->RateCount; i++) {
        if (Alt->Rates[i] == RateHz) {
            return TRUE;
        }
    }
    return FALSE;
}

const EMU_STREAM_ALT*
EmuCapsFindAlt(
    _In_ const EMU_DEVICE_CAPS* Caps,
    _In_ ULONG Direction,
    _In_ ULONG Channels,
    _In_ ULONG RateHz
    )
{
    for (ULONG i = 0; i < Caps->StreamAltCount; i++) {
        const EMU_STREAM_ALT* alt = &Caps->StreamAlts[i];
        if (alt->Direction == Direction && alt->Channels == Channels &&
            EmuAltSupportsRate(alt, RateHz)) {
            return alt;
        }
    }
    return NULL;
}

BOOLEAN
EmuCapsSupportsRate(_In_ const EMU_DEVICE_CAPS* Caps, _In_ ULONG RateHz)
{
    for (ULONG i = 0; i < Caps->StreamAltCount; i++) {
        if (EmuAltSupportsRate(&Caps->StreamAlts[i], RateHz)) {
            return TRUE;
        }
    }
    return FALSE;
}

ULONG
EmuCapsRateList(
    _In_ const EMU_DEVICE_CAPS* Caps,
    _Out_writes_(MaxRates) ULONG* Rates,
    _In_ ULONG MaxRates,
    _Out_ PULONG Flags
    )
{
    // Every rate in the table is at most RateCount per alt, and the parser
    // refuses a table that would exceed either bound, so the union cannot be
    // larger than this.
    ULONG collected[EMU_MAX_STREAM_ALTS * EMU_MAX_RATES_PER_ALT];
    ULONG count = 0;
    ULONG written;

    *Flags = 0;

    for (ULONG i = 0; i < Caps->StreamAltCount; i++) {
        const EMU_STREAM_ALT* alt = &Caps->StreamAlts[i];

        if (alt->RateFlags & EMU_ALT_CONTINUOUS_RATES) {
            *Flags |= EMU_RATES_CONTINUOUS;
        }
        for (ULONG j = 0; j < alt->RateCount; j++) {
            ULONG rate = alt->Rates[j];
            BOOLEAN known = FALSE;

            for (ULONG k = 0; k < count; k++) {
                if (collected[k] == rate) {
                    known = TRUE;
                    break;
                }
            }
            if (!known) {
                collected[count++] = rate;
            }
        }
    }

    // Insertion sort: the list is bounded by EMU_MAX_RATES_PER_ALT per alt, and
    // a device the parser accepted never has enough entries to care.
    for (ULONG i = 1; i < count; i++) {
        ULONG value = collected[i];
        LONG  j = (LONG)i;

        while (j > 0 && collected[j - 1] > value) {
            collected[j] = collected[j - 1];
            j--;
        }
        collected[j] = value;
    }

    written = (count > MaxRates) ? MaxRates : count;
    if (count > MaxRates) {
        *Flags |= EMU_RATES_TRUNCATED;
    }
    for (ULONG i = 0; i < written; i++) {
        Rates[i] = collected[i];
    }
    return written;
}
