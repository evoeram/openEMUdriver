// EmuUSBAudio — WaveRT render endpoint backed by USB isochronous completions.
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "emu_wave.h"
#include "emu_audio.h"

#define EMU_WAVE_PIN_SINK   0
#define EMU_WAVE_PIN_SOURCE 1
#define EMU_TOPO_PIN_SOURCE 0
#define EMU_TOPO_PIN_DEST   1

// --- Format, pin and filter descriptors --------------------------------------

static KSDATAFORMAT_WAVEFORMATEXTENSIBLE g_EmuWaveFormat =
{
    {
        sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    {
        {
            WAVE_FORMAT_EXTENSIBLE,
            EMU_AUDIO_M4_CHANNELS,
            EMU_AUDIO_M4_RATE_HZ,
            EMU_AUDIO_M4_RATE_HZ * EMU_AUDIO_M4_CHANNELS *
                EMU_AUDIO_HOST_SAMPLE_BYTES,
            EMU_AUDIO_M4_CHANNELS * EMU_AUDIO_HOST_SAMPLE_BYTES,
            32,
            sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)
        },
        24,
        KSAUDIO_SPEAKER_STEREO,
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
    }
};

static KSATTRIBUTE g_EmuModeAttribute =
{
    sizeof(KSATTRIBUTE),
    0,
    STATICGUIDOF(KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE)
};

static PKSATTRIBUTE g_EmuModeAttributes[] = { &g_EmuModeAttribute };
static KSATTRIBUTE_LIST g_EmuModeAttributeList =
{
    ARRAYSIZE(g_EmuModeAttributes),
    g_EmuModeAttributes
};

static KSDATARANGE_AUDIO g_EmuWaveStreamRange =
{
    {
        sizeof(KSDATARANGE_AUDIO),
        KSDATARANGE_ATTRIBUTES,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    EMU_AUDIO_M4_CHANNELS,
    EMU_AUDIO_M4_VALID_BITS,
    EMU_AUDIO_M4_VALID_BITS,
    EMU_AUDIO_M4_RATE_HZ,
    EMU_AUDIO_M4_RATE_HZ
};

static PKSDATARANGE g_EmuWaveStreamRanges[] =
{
    PKSDATARANGE(&g_EmuWaveStreamRange),
    PKSDATARANGE(&g_EmuModeAttributeList)
};

static KSDATARANGE g_EmuAnalogRange =
{
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};

static PKSDATARANGE g_EmuAnalogRanges[] = { &g_EmuAnalogRange };

static PCPIN_DESCRIPTOR g_EmuWavePins[] =
{
    {
        1, 1, 0, NULL,
        {
            0, NULL, 0, NULL,
            ARRAYSIZE(g_EmuWaveStreamRanges), g_EmuWaveStreamRanges,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK,
            &KSCATEGORY_AUDIO, NULL, 0
        }
    },
    {
        0, 0, 0, NULL,
        {
            0, NULL, 0, NULL,
            ARRAYSIZE(g_EmuAnalogRanges), g_EmuAnalogRanges,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO, NULL, 0
        }
    }
};

static PCCONNECTION_DESCRIPTOR g_EmuWaveConnections[] =
{
    { PCFILTER_NODE, EMU_WAVE_PIN_SINK, PCFILTER_NODE, EMU_WAVE_PIN_SOURCE }
};

static NTSTATUS EmuWaveFilterProperty(_In_ PPCPROPERTY_REQUEST Request);

static PCPROPERTY_ITEM g_EmuWaveProperties[] =
{
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        EmuWaveFilterProperty
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        EmuWaveFilterProperty
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(g_EmuWaveAutomation, g_EmuWaveProperties);

static PCFILTER_DESCRIPTOR g_EmuWaveDescriptor =
{
    0,
    &g_EmuWaveAutomation,
    sizeof(PCPIN_DESCRIPTOR),
    ARRAYSIZE(g_EmuWavePins),
    g_EmuWavePins,
    sizeof(PCNODE_DESCRIPTOR),
    0,
    NULL,
    ARRAYSIZE(g_EmuWaveConnections),
    g_EmuWaveConnections,
    0,
    NULL
};

static PCPIN_DESCRIPTOR g_EmuTopologyPins[] =
{
    {
        0, 0, 0, NULL,
        {
            0, NULL, 0, NULL,
            ARRAYSIZE(g_EmuAnalogRanges), g_EmuAnalogRanges,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO, NULL, 0
        }
    },
    {
        0, 0, 0, NULL,
        {
            0, NULL, 0, NULL,
            ARRAYSIZE(g_EmuAnalogRanges), g_EmuAnalogRanges,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
            &KSNODETYPE_SPEAKER, NULL, 0
        }
    }
};

static PCCONNECTION_DESCRIPTOR g_EmuTopologyConnections[] =
{
    { PCFILTER_NODE, EMU_TOPO_PIN_SOURCE, PCFILTER_NODE, EMU_TOPO_PIN_DEST }
};

static PCFILTER_DESCRIPTOR g_EmuTopologyDescriptor =
{
    0,
    NULL,
    sizeof(PCPIN_DESCRIPTOR),
    ARRAYSIZE(g_EmuTopologyPins),
    g_EmuTopologyPins,
    sizeof(PCNODE_DESCRIPTOR),
    0,
    NULL,
    ARRAYSIZE(g_EmuTopologyConnections),
    g_EmuTopologyConnections,
    0,
    NULL
};

// --- Format and attribute validation ----------------------------------------

#pragma code_seg("PAGE")
static NTSTATUS EmuWaveBasicSupport(
    _In_ PPCPROPERTY_REQUEST Request,
    _In_ ULONG Flags
    )
{
    PAGED_CODE();

    if (Request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
        PKSPROPERTY_DESCRIPTION description =
            (PKSPROPERTY_DESCRIPTION)Request->Value;
        description->AccessFlags = Flags;
        description->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
        description->PropTypeSet.Set = GUID_NULL;
        description->PropTypeSet.Id = 0;
        description->PropTypeSet.Flags = 0;
        description->MembersListCount = 0;
        description->Reserved = 0;
        Request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        return STATUS_SUCCESS;
    }
    if (Request->ValueSize >= sizeof(ULONG)) {
        *(PULONG)Request->Value = Flags;
        Request->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    Request->ValueSize = 0;
    return STATUS_BUFFER_TOO_SMALL;
}

#pragma code_seg("PAGE")
static NTSTATUS EmuWaveModeFromAttributes(
    _In_ const KSMULTIPLE_ITEM* Items,
    _In_ SIZE_T Bytes,
    _Out_ GUID* Mode
    )
{
    SIZE_T remaining;
    PKSATTRIBUTE attribute;

    PAGED_CODE();

    *Mode = AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
    if (Items == NULL || Bytes < sizeof(KSMULTIPLE_ITEM) ||
        Items->Size < sizeof(KSMULTIPLE_ITEM) || Items->Size > Bytes) {
        return STATUS_INVALID_PARAMETER;
    }

    remaining = Items->Size - sizeof(KSMULTIPLE_ITEM);
    attribute = (PKSATTRIBUTE)(Items + 1);
    for (ULONG i = 0; i < Items->Count; i++) {
        ULONG aligned;

        if (remaining < sizeof(KSATTRIBUTE)) {
            return STATUS_INVALID_PARAMETER;
        }
        if (!IsEqualGUIDAligned(attribute->Attribute,
                                KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE)) {
            return STATUS_NOT_SUPPORTED;
        }
        if (attribute->Size != sizeof(KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE) ||
            remaining < sizeof(KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE)) {
            return STATUS_INVALID_PARAMETER;
        }
        *Mode = ((PKSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE)attribute)->
            SignalProcessingMode;
        aligned = (attribute->Size + FILE_QUAD_ALIGNMENT) & ~FILE_QUAD_ALIGNMENT;
        if (aligned > remaining) {
            return STATUS_INVALID_PARAMETER;
        }
        remaining -= aligned;
        attribute = (PKSATTRIBUTE)((PUCHAR)attribute + aligned);
    }
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
static NTSTATUS EmuWaveValidateFormat(_In_ PKSDATAFORMAT DataFormat)
{
    PWAVEFORMATEX wave;
    PWAVEFORMATEXTENSIBLE ext;

    PAGED_CODE();

    if (DataFormat == NULL ||
        DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(DataFormat->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(DataFormat->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(DataFormat->Specifier,
                            KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        return STATUS_NO_MATCH;
    }

    wave = (PWAVEFORMATEX)(DataFormat + 1);
    if (wave->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        DataFormat->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) ||
        wave->nChannels != EMU_AUDIO_M4_CHANNELS ||
        wave->nSamplesPerSec != EMU_AUDIO_M4_RATE_HZ ||
        wave->nAvgBytesPerSec != EMU_AUDIO_M4_RATE_HZ *
            EMU_AUDIO_M4_CHANNELS * EMU_AUDIO_HOST_SAMPLE_BYTES ||
        wave->nBlockAlign != EMU_AUDIO_M4_CHANNELS *
            EMU_AUDIO_HOST_SAMPLE_BYTES ||
        wave->wBitsPerSample != 32 ||
        wave->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return STATUS_NO_MATCH;
    }

    ext = (PWAVEFORMATEXTENSIBLE)wave;
    if (ext->Samples.wValidBitsPerSample != 24 ||
        ext->dwChannelMask != KSAUDIO_SPEAKER_STEREO ||
        !IsEqualGUIDAligned(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
        return STATUS_NO_MATCH;
    }
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
static NTSTATUS EmuWaveValidateMode(_In_ PKSDATAFORMAT DataFormat)
{
    GUID mode;
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE();

    mode = AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
    if (DataFormat->Flags & KSDATAFORMAT_ATTRIBUTES) {
        PKSMULTIPLE_ITEM items = (PKSMULTIPLE_ITEM)((PUCHAR)DataFormat +
            ((DataFormat->FormatSize + FILE_QUAD_ALIGNMENT) &
             ~FILE_QUAD_ALIGNMENT));
        status = EmuWaveModeFromAttributes(items, items->Size, &mode);
    }
    if (NT_SUCCESS(status) &&
        !IsEqualGUID(mode, AUDIO_SIGNALPROCESSINGMODE_DEFAULT) &&
        !IsEqualGUID(mode, AUDIO_SIGNALPROCESSINGMODE_RAW)) {
        status = STATUS_NOT_SUPPORTED;
    }
    return status;
}

// --- Adapter power object ----------------------------------------------------

STDMETHODIMP_(VOID) CEmuAdapter::PowerChangeState(_In_ POWER_STATE NewState)
{
    // PortCls pauses streams before sleep. Their RUN -> PAUSE transition stops
    // the USB engine, so there is no hardware access left to defer here.
    UNREFERENCED_PARAMETER(NewState);
}

STDMETHODIMP_(NTSTATUS) CEmuAdapter::QueryPowerChangeState(
    _In_ POWER_STATE NewStateQuery
    )
{
    UNREFERENCED_PARAMETER(NewStateQuery);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CEmuAdapter::QueryDeviceCapabilities(
    _Inout_ PDEVICE_CAPABILITIES PowerDeviceCaps
    )
{
    UNREFERENCED_PARAMETER(PowerDeviceCaps);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP CEmuAdapter::NonDelegatingQueryInterface(
    _In_ REFIID Interface,
    _COM_Outptr_ PVOID* Object
    )
{
    PAGED_CODE();

    if (Object == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Object = NULL;
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PADAPTERPOWERMANAGEMENT(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement)) {
        *Object = PVOID(PADAPTERPOWERMANAGEMENT(this));
    }
    if (*Object != NULL) {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

// --- Wave miniport -----------------------------------------------------------

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveMiniport::Init(
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTWAVERT Port
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    if (UnknownAdapter == NULL || m_Context->Caps == NULL ||
        EmuCapsFindAlt(m_Context->Caps, EMU_STREAM_RENDER,
                       EMU_AUDIO_M4_CHANNELS, EMU_AUDIO_M4_RATE_HZ) == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveMiniport::GetDescription(
    _Out_ PPCFILTER_DESCRIPTOR* OutFilterDescriptor
    )
{
    PAGED_CODE();

    if (OutFilterDescriptor == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *OutFilterDescriptor = &g_EmuWaveDescriptor;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
static NTSTATUS EmuWaveDataRangeIntersectionBody(
    _In_ ULONG PinId,
    _In_ PKSDATARANGE ClientDataRange,
    _In_ PKSDATARANGE MyDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
        PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(MyDataRange);

    if (ResultantFormatLength == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *ResultantFormatLength = 0;

    // The generic PortCls intersection builds a plain packed WAVEFORMATEX out
    // of the data range, and the audio engine has no float->packed-24 or
    // float->int-32 converter, so an intersection that defers to it leaves the
    // endpoint without a mix format (GetMixFormat = AUDCLNT_E_UNSUPPORTED_
    // FORMAT). Return the exact 24-in-32 device format instead, like the
    // in-box HDAudio miniport does.
    if (!IsEqualGUIDAligned(ClientDataRange->Specifier,
                            KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
        !IsEqualGUIDAligned(ClientDataRange->Specifier,
                            KSDATAFORMAT_SPECIFIER_WILDCARD)) {
        return STATUS_NO_MATCH;
    }
    // The engine probes the intersection with float32 client ranges (its mix
    // pipeline is float); answering NO_MATCH to those defers to the generic
    // PortCls intersection, whose packed-PCM result the engine cannot use.
    // Accept float clients and return the closest device format, like the
    // in-box HDAudio miniport does.
    if (!IsEqualGUIDAligned(ClientDataRange->SubFormat,
                            KSDATAFORMAT_SUBTYPE_WILDCARD) &&
        !IsEqualGUIDAligned(ClientDataRange->SubFormat,
                            KSDATAFORMAT_SUBTYPE_PCM) &&
        !IsEqualGUIDAligned(ClientDataRange->SubFormat,
                            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
        return STATUS_NO_MATCH;
    }
    if (ClientDataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
        PKSDATARANGE_AUDIO client = (PKSDATARANGE_AUDIO)ClientDataRange;
        if (client->MaximumChannels != 0xFFFFFFFF &&
            client->MaximumChannels < EMU_AUDIO_M4_CHANNELS) {
            return STATUS_NO_MATCH;
        }
        if (client->MinimumSampleFrequency > EMU_AUDIO_M4_RATE_HZ ||
            client->MaximumSampleFrequency < EMU_AUDIO_M4_RATE_HZ) {
            return STATUS_NO_MATCH;
        }
    }

    ULONG required = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    *ResultantFormatLength = required;
    if (OutputBufferLength == 0) {
        // Size query: ResultantFormat may legitimately be NULL here.
        return STATUS_BUFFER_OVERFLOW;
    }
    if (ResultantFormat == NULL || OutputBufferLength < required) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(ResultantFormat, &g_EmuWaveFormat, required);
    return STATUS_SUCCESS;
}

// Diagnostics for the M4 format-acquisition gate: the ISO transport is proven,
// so if the endpoint still has no mix format these breadcrumbs show whether
// the audio stack reaches the intersection at all, with which client ranges,
// and what we answer.
static ULONG g_DriCalls;
static ULONG g_DriLastClientSub;
static ULONG g_DriLastClientCh;
static ULONG g_DriLastStatus;

// Silence probe: what actually left the host in the first packet after RUN.
// Armed by SetState(KSSTATE_RUN) (PASSIVE), sampled by FillUsbPacket
// (DISPATCH_LEVEL), flushed to the registry by GetPosition (PASSIVE).
extern const char g_OedProbeMark[] = "OEDPROBE46";  // ASCII build marker of the probe source
static volatile LONG g_FillProbeDirty;
static ULONG g_FillNonZero;
static ULONG g_FillSig0;
static ULONG g_FillSig1;
static ULONG g_HostNonZero;
static ULONG g_HostSig0;
static ULONG g_HostSig1;
static ULONG g_ProbeSampled;   // LinearFramePosition low 32 of the sampled fill
static ULONG g_FillCalls;   // FillUsbPacket invocations - zero means the pattern feeds the wire
static ULONG g_ArmTicks;       // SetState(RUN) arm executions, fresh per boot
static volatile LONG g_FillSeen;      // one-shot: first FillUsbPacket entry
static volatile LONG g_FillRejected;  // one-shot: first validation rejection
static volatile LONG g_PosIrql;       // highest IRQL GetPosition was seen at
static ULONG g_Underruns;
// SetWritePacket verdicts: the pump's only data source is the OS's release
// path, so its acceptance record is the difference between "wire silent
// because the engine never released" and "wire silent because we rejected".
static volatile LONG g_SwrSeen;    // one-shot: first accepted release
static ULONG g_SwrLate;            // DATA_LATE_ERROR answers
static ULONG g_SwrOver;            // DATA_OVERRUN answers
static volatile LONG g_NotifKick;  // one-shot: the RUN bootstrap signal fired
// Fill rejection forensics (flushed at STOP; registry writes are PASSIVE-only
// and the pump fills at DISPATCH too): why the last rejected fill returned 0.
static ULONG g_FillRejectReason;   // 1 = validation, 2 = ring spans, 3 = pack
static ULONGLONG g_FillRejectPos;  // LinearFramePosition of the rejection
static ULONG g_FillBufferBytes;    // the WaveRT ring size the fill lives by

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveMiniport::DataRangeIntersection(
    _In_ ULONG PinId,
    _In_ PKSDATARANGE ClientDataRange,
    _In_ PKSDATARANGE MyDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
        PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength
    )
{
    NTSTATUS status;

    PAGED_CODE();

    status = EmuWaveDataRangeIntersectionBody(PinId, ClientDataRange,
                                              MyDataRange, OutputBufferLength,
                                              ResultantFormat,
                                              ResultantFormatLength);
    g_DriCalls++;
    g_DriLastClientSub = (ClientDataRange != NULL)
                             ? ClientDataRange->SubFormat.Data1 : 0;
    g_DriLastClientCh =
        (ClientDataRange != NULL &&
         ClientDataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO))
            ? ((PKSDATARANGE_AUDIO)ClientDataRange)->MaximumChannels
            : 0xFFFFFFFF;
    g_DriLastStatus = (ULONG)status;
    EmuNoteStatusValue(NULL, L"DriCalls", g_DriCalls);
    EmuNoteStatusValue(NULL, L"DriLastClientSub", g_DriLastClientSub);
    EmuNoteStatusValue(NULL, L"DriLastClientCh", g_DriLastClientCh);
    EmuNoteStatusValue(NULL, L"DriLastStatus", g_DriLastStatus);
    return status;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveMiniport::NewStream(
    _Out_ PMINIPORTWAVERTSTREAM* OutStream,
    _In_ PPORTWAVERTSTREAM OuterUnknown,
    _In_ ULONG Pin,
    _In_ BOOLEAN Capture,
    _In_ PKSDATAFORMAT DataFormat
    )
{
    CEmuWaveStream* stream;
    NTSTATUS status;

    PAGED_CODE();

    if (OutStream == NULL || OuterUnknown == NULL || DataFormat == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *OutStream = NULL;
    if (Pin != EMU_WAVE_PIN_SINK || Capture) {
        return STATUS_INVALID_PARAMETER;
    }
    status = EmuWaveValidateFormat(DataFormat);
    if (NT_SUCCESS(status)) {
        status = EmuWaveValidateMode(DataFormat);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (InterlockedCompareExchange(&m_StreamAllocated, 1, 0) != 0) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    stream = new (POOL_FLAG_NON_PAGED, EMU_POOLTAG) CEmuWaveStream(NULL);
    if (stream == NULL) {
        InterlockedExchange(&m_StreamAllocated, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    stream->AddRef();
    status = stream->Init(this, OuterUnknown);
    if (NT_SUCCESS(status)) {
        *OutStream = PMINIPORTWAVERTSTREAM(stream);
        (*OutStream)->AddRef();
    }
    stream->Release();
    return status;
}

#pragma code_seg("PAGE")
STDMETHODIMP CEmuWaveMiniport::NonDelegatingQueryInterface(
    _In_ REFIID Interface,
    _COM_Outptr_ PVOID* Object
    )
{
    PAGED_CODE();

    if (Object == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Object = NULL;
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniport)) {
        *Object = PVOID(PMINIPORT(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT)) {
        *Object = PVOID(PMINIPORTWAVERT(this));
    }
    if (*Object != NULL) {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveMiniport::GetDeviceDescription(
    _Out_ PDEVICE_DESCRIPTION DmaDeviceDescription
    )
{
    PAGED_CODE();

    if (DmaDeviceDescription == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(DmaDeviceDescription, sizeof(DEVICE_DESCRIPTION));
    DmaDeviceDescription->Master = TRUE;
    DmaDeviceDescription->ScatterGather = TRUE;
    DmaDeviceDescription->Dma32BitAddresses = TRUE;
    DmaDeviceDescription->InterfaceType = PCIBus;
    DmaDeviceDescription->MaximumLength = MAXULONG;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
NTSTATUS CEmuWaveMiniport::PropertyHandlerProposedFormat(
    _In_ PPCPROPERTY_REQUEST Request
    )
{
    PKSP_PIN pin;

    PAGED_CODE();

    if (Request->InstanceSize <
        sizeof(KSP_PIN) - RTL_SIZEOF_THROUGH_FIELD(KSP_PIN, Property)) {
        return STATUS_INVALID_PARAMETER;
    }
    pin = CONTAINING_RECORD(Request->Instance, KSP_PIN, PinId);
    if (pin->PinId != EMU_WAVE_PIN_SINK) {
        return STATUS_NOT_SUPPORTED;
    }
    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        return EmuWaveBasicSupport(Request, Request->PropertyItem->Flags);
    }
    if (Request->ValueSize == 0) {
        Request->ValueSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
        return STATUS_BUFFER_OVERFLOW;
    }
    if (Request->ValueSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if ((Request->Verb & KSPROPERTY_TYPE_SET) == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    return EmuWaveValidateFormat((PKSDATAFORMAT)Request->Value);
}

#pragma code_seg("PAGE")
NTSTATUS CEmuWaveMiniport::PropertyHandlerProposedFormat2(
    _In_ PPCPROPERTY_REQUEST Request
    )
{
    PKSP_PIN pin;
    PKSMULTIPLE_ITEM inputItems;
    SIZE_T attributeBytes;
    ULONG alignedFormat;
    ULONG required;
    GUID mode;
    NTSTATUS status;

    PAGED_CODE();

    if (Request->InstanceSize <
        sizeof(KSP_PIN) - RTL_SIZEOF_THROUGH_FIELD(KSP_PIN, Property)) {
        return STATUS_INVALID_PARAMETER;
    }
    pin = CONTAINING_RECORD(Request->Instance, KSP_PIN, PinId);
    if (pin->PinId != EMU_WAVE_PIN_SINK) {
        return STATUS_NOT_SUPPORTED;
    }
    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        return EmuWaveBasicSupport(Request, Request->PropertyItem->Flags);
    }

    inputItems = (PKSMULTIPLE_ITEM)(pin + 1);
    attributeBytes = ((PUCHAR)Request->Instance + Request->InstanceSize) -
        (PUCHAR)inputItems;
    status = EmuWaveModeFromAttributes(inputItems, attributeBytes, &mode);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (!IsEqualGUID(mode, AUDIO_SIGNALPROCESSINGMODE_DEFAULT) &&
        !IsEqualGUID(mode, AUDIO_SIGNALPROCESSINGMODE_RAW)) {
        return STATUS_NOT_SUPPORTED;
    }
    if ((Request->Verb & KSPROPERTY_TYPE_GET) == 0 ||
        attributeBytes > MAXULONG) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    alignedFormat = (sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) +
        FILE_QUAD_ALIGNMENT) & ~FILE_QUAD_ALIGNMENT;
    required = alignedFormat + (ULONG)attributeBytes;
    if (Request->ValueSize == 0) {
        Request->ValueSize = required;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (Request->ValueSize < required) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(Request->Value, &g_EmuWaveFormat, sizeof(g_EmuWaveFormat));
    ((PKSDATAFORMAT)Request->Value)->Flags = KSDATAFORMAT_ATTRIBUTES;
    RtlCopyMemory((PUCHAR)Request->Value + alignedFormat,
                  inputItems, attributeBytes);
    Request->ValueSize = required;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
static NTSTATUS EmuWaveFilterProperty(_In_ PPCPROPERTY_REQUEST Request)
{
    CEmuWaveMiniport* miniport;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    static ULONG s_PropCalls;
    static ULONG s_PropLastId;
    static ULONG s_PropLastStatus;

    PAGED_CODE();

    miniport = (CEmuWaveMiniport*)Request->MajorTarget;
    if (miniport == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    miniport->AddRef();
    if (IsEqualGUIDAligned(*Request->PropertyItem->Set, KSPROPSETID_Pin)) {
        if (Request->PropertyItem->Id == KSPROPERTY_PIN_PROPOSEDATAFORMAT) {
            status = miniport->PropertyHandlerProposedFormat(Request);
        } else if (Request->PropertyItem->Id == KSPROPERTY_PIN_PROPOSEDATAFORMAT2) {
            status = miniport->PropertyHandlerProposedFormat2(Request);
        }
    }
    miniport->Release();
    s_PropCalls++;
    s_PropLastId = Request->PropertyItem->Id;
    s_PropLastStatus = (ULONG)status;
    EmuNoteStatusValue(NULL, L"PropCalls", s_PropCalls);
    EmuNoteStatusValue(NULL, L"PropLastId", s_PropLastId);
    EmuNoteStatusValue(NULL, L"PropLastStatus", s_PropLastStatus);
    return status;
}

// --- Topology miniport -------------------------------------------------------

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuTopologyMiniport::Init(
    _In_ PUNKNOWN UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTTOPOLOGY Port
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ResourceList);
    return (UnknownAdapter != NULL && Port != NULL) ?
        STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuTopologyMiniport::GetDescription(
    _Out_ PPCFILTER_DESCRIPTOR* OutFilterDescriptor
    )
{
    PAGED_CODE();

    if (OutFilterDescriptor == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *OutFilterDescriptor = &g_EmuTopologyDescriptor;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuTopologyMiniport::DataRangeIntersection(
    _In_ ULONG PinId,
    _In_ PKSDATARANGE ClientDataRange,
    _In_ PKSDATARANGE MyDataRange,
    _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
        PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(ClientDataRange);
    UNREFERENCED_PARAMETER(MyDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;
}

#pragma code_seg("PAGE")
STDMETHODIMP CEmuTopologyMiniport::NonDelegatingQueryInterface(
    _In_ REFIID Interface,
    _COM_Outptr_ PVOID* Object
    )
{
    PAGED_CODE();

    if (Object == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Object = NULL;
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniport)) {
        *Object = PVOID(PMINIPORT(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology)) {
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
    }
    if (*Object != NULL) {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

// --- WaveRT stream -----------------------------------------------------------

CEmuWaveStream::CEmuWaveStream(_In_opt_ PUNKNOWN Outer)
    : CUnknown(Outer),
      m_Miniport(NULL),
      m_Context(NULL),
      m_PortStream(NULL),
      m_Buffer(NULL),
      m_BufferMdl(NULL),
      m_BufferBytes(0),
      m_NotificationCount(0),
      m_NotificationFrames(0),
      m_State(KSSTATE_STOP),
      m_BaseFrames(0),
      m_NextNotificationFrame(0),
      m_LastOsWriteAbs(0)
{
    InitializeListHead(&m_NotificationList);
    KeInitializeSpinLock(&m_NotificationLock);
}

#pragma code_seg("PAGE")
NTSTATUS CEmuWaveStream::Init(
    _In_ CEmuWaveMiniport* Miniport,
    _In_ PPORTWAVERTSTREAM PortStream
    )
{
    PAGED_CODE();

    if (Miniport == NULL || PortStream == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    m_Miniport = Miniport;
    m_Miniport->AddRef();
    m_Context = Miniport->m_Context;
    m_PortStream = PortStream;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
CEmuWaveStream::~CEmuWaveStream()
{
    PLIST_ENTRY entry;

    PAGED_CODE();

    StopAndReset();
    FreeBuffer(m_BufferMdl);

    // The USB worker is gone, so no callback can race list teardown now.
    while (!IsListEmpty(&m_NotificationList)) {
        PEMU_WAVE_NOTIFICATION notification;
        entry = RemoveHeadList(&m_NotificationList);
        notification = CONTAINING_RECORD(entry, EMU_WAVE_NOTIFICATION, Link);
        ExFreePoolWithTag(notification, EMU_POOLTAG);
    }
    if (m_Miniport != NULL) {
        InterlockedExchange(&m_Miniport->m_StreamAllocated, 0);
        m_Miniport->Release();
        m_Miniport = NULL;
    }
}

#pragma code_seg("PAGE")
STDMETHODIMP CEmuWaveStream::NonDelegatingQueryInterface(
    _In_ REFIID Interface,
    _COM_Outptr_ PVOID* Object
    )
{
    PAGED_CODE();

    if (Object == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Object = NULL;
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream)) {
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    } else if (IsEqualGUIDAligned(Interface,
                                  IID_IMiniportWaveRTStreamNotification)) {
        *Object = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTOutputStream)) {
        *Object = PVOID(PMINIPORTWAVERTOUTPUTSTREAM(this));
    }
    if (*Object != NULL) {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

#pragma code_seg("PAGE")
NTSTATUS CEmuWaveStream::AllocateBuffer(
    _In_ ULONG NotificationCount,
    _In_ ULONG RequestedSize,
    _Out_ PMDL* AudioBufferMdl,
    _Out_ PULONG ActualSize,
    _Out_ PULONG OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE* CacheType
    )
{
    PHYSICAL_ADDRESS highAddress;
    PMDL mdl;

    PAGED_CODE();

    if (AudioBufferMdl == NULL || ActualSize == NULL ||
        OffsetFromFirstPage == NULL || CacheType == NULL ||
        m_Buffer != NULL || m_State != KSSTATE_STOP) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    RequestedSize -= RequestedSize % (EMU_AUDIO_M4_CHANNELS *
                                      EMU_AUDIO_HOST_SAMPLE_BYTES);
    if (NotificationCount != 0 &&
        ((RequestedSize % NotificationCount) != 0 ||
         ((RequestedSize / NotificationCount) %
          (EMU_AUDIO_M4_CHANNELS * EMU_AUDIO_HOST_SAMPLE_BYTES)) != 0)) {
        // Both the whole ring and every notification period must contain an
        // integral number of host frames after alignment.
        return STATUS_INVALID_PARAMETER;
    }
    // One USB packet must always fit in the host ring; otherwise a single pack
    // request would need more than the two physical spans a ring can provide.
    if (RequestedSize < EMU_AUDIO_M4_RATE_HZ / 1000 *
        EMU_AUDIO_M4_CHANNELS * EMU_AUDIO_HOST_SAMPLE_BYTES) {
        return STATUS_INVALID_PARAMETER;
    }

    highAddress.QuadPart = MAXULONG;
    mdl = m_PortStream->AllocatePagesForMdl(highAddress, RequestedSize);
    if (mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    m_Buffer = (PUCHAR)m_PortStream->MapAllocatedPages(mdl, MmCached);
    if (m_Buffer == NULL) {
        m_PortStream->FreePagesFromMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    m_BufferMdl = mdl;
    m_BufferBytes = RequestedSize;
    m_NotificationCount = NotificationCount;
    m_NotificationFrames = NotificationCount != 0 ?
        RequestedSize / NotificationCount /
        (EMU_AUDIO_M4_CHANNELS * EMU_AUDIO_HOST_SAMPLE_BYTES) : 0;
    if (NotificationCount != 0 && m_NotificationFrames == 0) {
        FreeBuffer(mdl);
        return STATUS_INVALID_PARAMETER;
    }
    m_NextNotificationFrame = m_NotificationFrames;
    *AudioBufferMdl = mdl;
    *ActualSize = RequestedSize;
    *OffsetFromFirstPage = 0;
    *CacheType = MmCached;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::AllocateAudioBuffer(
    _In_ ULONG RequestedSize,
    _Out_ PMDL* AudioBufferMdl,
    _Out_ PULONG ActualSize,
    _Out_ PULONG OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE* CacheType
    )
{
    PAGED_CODE();

    return AllocateBuffer(0, RequestedSize, AudioBufferMdl, ActualSize,
                          OffsetFromFirstPage, CacheType);
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::AllocateBufferWithNotification(
    _In_ ULONG NotificationCount,
    _In_ ULONG RequestedSize,
    _Out_ PMDL* AudioBufferMdl,
    _Out_ PULONG ActualSize,
    _Out_ PULONG OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE* CacheType
    )
{
    PAGED_CODE();

    if (NotificationCount == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    return AllocateBuffer(NotificationCount, RequestedSize, AudioBufferMdl,
                          ActualSize, OffsetFromFirstPage, CacheType);
}

#pragma code_seg("PAGE")
VOID CEmuWaveStream::FreeBuffer(_In_opt_ PMDL Mdl)
{
    PMDL mdl = Mdl != NULL ? Mdl : m_BufferMdl;

    PAGED_CODE();

    StopAndReset();
    if (mdl != NULL) {
        if (m_Buffer != NULL) {
            m_PortStream->UnmapAllocatedPages(m_Buffer, mdl);
            m_Buffer = NULL;
        }
        m_PortStream->FreePagesFromMdl(mdl);
    }
    m_BufferMdl = NULL;
    m_BufferBytes = 0;
    m_NotificationCount = 0;
    m_NotificationFrames = 0;
    m_NextNotificationFrame = 0;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(VOID) CEmuWaveStream::FreeAudioBuffer(
    _In_opt_ PMDL Mdl,
    _In_ ULONG Size
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Size);
    FreeBuffer(Mdl);
}

#pragma code_seg("PAGE")
STDMETHODIMP_(VOID) CEmuWaveStream::FreeBufferWithNotification(
    _In_ PMDL Mdl,
    _In_ ULONG Size
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Size);
    FreeBuffer(Mdl);
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::RegisterNotificationEvent(
    _In_ PKEVENT NotificationEvent
    )
{
    PEMU_WAVE_NOTIFICATION notification;
    PLIST_ENTRY entry;
    KIRQL oldIrql;
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE();

    if (NotificationEvent == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    notification = (PEMU_WAVE_NOTIFICATION)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*notification), EMU_POOLTAG);
    if (notification == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    notification->Event = NotificationEvent;

    KeAcquireSpinLock(&m_NotificationLock, &oldIrql);
    for (entry = m_NotificationList.Flink; entry != &m_NotificationList;
         entry = entry->Flink) {
        PEMU_WAVE_NOTIFICATION current = CONTAINING_RECORD(
            entry, EMU_WAVE_NOTIFICATION, Link);
        if (current->Event == NotificationEvent) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
    }
    if (NT_SUCCESS(status)) {
        InsertTailList(&m_NotificationList, &notification->Link);
    }
    KeReleaseSpinLock(&m_NotificationLock, oldIrql);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(notification, EMU_POOLTAG);
    }
    return status;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::UnregisterNotificationEvent(
    _In_ PKEVENT NotificationEvent
    )
{
    PLIST_ENTRY entry;
    KIRQL oldIrql;
    PEMU_WAVE_NOTIFICATION found = NULL;

    PAGED_CODE();

    KeAcquireSpinLock(&m_NotificationLock, &oldIrql);
    for (entry = m_NotificationList.Flink; entry != &m_NotificationList;
         entry = entry->Flink) {
        PEMU_WAVE_NOTIFICATION current = CONTAINING_RECORD(
            entry, EMU_WAVE_NOTIFICATION, Link);
        if (current->Event == NotificationEvent) {
            found = current;
            RemoveEntryList(entry);
            break;
        }
    }
    KeReleaseSpinLock(&m_NotificationLock, oldIrql);
    if (found == NULL) {
        return STATUS_NOT_FOUND;
    }
    ExFreePoolWithTag(found, EMU_POOLTAG);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::GetClockRegister(
    _Out_ PKSRTAUDIO_HWREGISTER Register
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_IMPLEMENTED;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::GetPositionRegister(
    _Out_ PKSRTAUDIO_HWREGISTER Register
    )
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_IMPLEMENTED;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(VOID) CEmuWaveStream::GetHWLatency(
    _Out_ PKSRTAUDIO_HWLATENCY Latency
    )
{
    PAGED_CODE();

    if (Latency != NULL) {
        Latency->ChipsetDelay = 0;
        Latency->CodecDelay = 0;
        Latency->FifoSize = EMU_ISOCH_SLOTS * (EMU_AUDIO_M4_RATE_HZ / 1000) *
            EMU_AUDIO_M4_CHANNELS * EMU_AUDIO_HOST_SAMPLE_BYTES;
    }
}

#pragma code_seg()
ULONGLONG CEmuWaveStream::TotalCompletedFrames()
{
    // Active, rather than the KS state, owns the current ISO counter. During
    // RUN -> PAUSE the counter is folded into BaseFrames after Active clears;
    // this ordering prevents a concurrent GetPosition from counting it twice.
    if (m_Context->Isoch[EMU_STREAM_RENDER].Active) {
        return m_BaseFrames + EmuIsochRenderCompletedFrames(m_Context);
    }
    return m_BaseFrames;
}

#pragma code_seg()
ULONGLONG CEmuWaveStream::TotalSubmittedFrames()
{
    // Frames handed to the USB wire (packed), completed or still in flight.
    // The packet model reports this to the OS: the engine must fill the ring
    // ahead of the CONSUMED cursor, or the 4-slot pipeline reads packets the
    // engine has not written yet.
    if (m_Context->Isoch[EMU_STREAM_RENDER].Active) {
        return m_BaseFrames + EmuIsochRenderSubmittedFrames(m_Context);
    }
    return m_BaseFrames;
}

#pragma code_seg()
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::GetPosition(
    _Out_ KSAUDIO_POSITION* Position
    )
{
    ULONGLONG linear;
    ULONG ring;

    if (Position == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (m_BufferBytes == 0) {
        Position->PlayOffset = 0;
        Position->WriteOffset = 0;
        return STATUS_SUCCESS;
    }
    if (!EmuAudioPositions(TotalCompletedFrames(), m_BufferBytes,
                           EMU_AUDIO_M4_CHANNELS * EMU_AUDIO_HOST_SAMPLE_BYTES,
                           &linear, &ring)) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    Position->PlayOffset = ring;
    // WriteOffset = the CONSUMED cursor: the engine must fill the ring beyond
    // the ISO pipeline's read cursor, not beyond the play cursor.
    if (!EmuAudioPositions(TotalSubmittedFrames(), m_BufferBytes,
                           EMU_AUDIO_M4_CHANNELS * EMU_AUDIO_HOST_SAMPLE_BYTES,
                           &linear, &ring)) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    Position->WriteOffset = ring;

    // Flush the silence probe (PASSIVE): what left the host in the first
    // packet after the last RUN transition. GetPosition is reachable at
    // DISPATCH_LEVEL; there the consume must not happen - the registry write
    // would fail silently and the probe data would be lost for good. The
    // whole flush tail is registry writes, so it sits behind the same gate.
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        InterlockedExchange(&g_PosIrql, (LONG)KeGetCurrentIrql());
    } else {
        if (InterlockedCompareExchange(&g_FillProbeDirty, 0, 1) == 1) {
            EmuNoteStatusValue(NULL, L"PosFlush", 0x10000001);
            EmuNoteStatusValue(NULL, L"FillNonZero", g_FillNonZero);
            EmuNoteStatusValue(NULL, L"FillSig0", g_FillSig0);
            EmuNoteStatusValue(NULL, L"FillSig1", g_FillSig1);
            EmuNoteStatusValue(NULL, L"HostNonZero", g_HostNonZero);
            EmuNoteStatusValue(NULL, L"HostSig0", g_HostSig0);
            EmuNoteStatusValue(NULL, L"HostSig1", g_HostSig1);
            EmuNoteStatusValue(NULL, L"ProbeSampled", g_ProbeSampled);
        }
        EmuNoteStatusValue(NULL, L"FillCalls", g_FillCalls);
        EmuNoteStatusValue(NULL, L"ArmTicks", g_ArmTicks);
        EmuNoteStatusValue(NULL, L"Underruns", g_Underruns);
        EmuNoteStatusValue(NULL, L"PosIrql", g_PosIrql);
    }
    return STATUS_SUCCESS;
}

#pragma code_seg()
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::SetWritePacket(
    _In_ ULONG PacketNumber,
    _In_ DWORD Flags,
    _In_ ULONG EosPacketLength
    )
{
    LONG submittedAbs;
    LONG ahead;
    LONG delta;

    UNREFERENCED_PARAMETER(EosPacketLength);

    if (m_NotificationCount == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    if ((Flags & KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (m_State != KSSTATE_RUN && m_State != KSSTATE_PAUSE) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    // The OS passes the cyclic ring-packet index; reconstruct the absolute
    // packet index by unwrapping forward from the last accepted one.
    submittedAbs = (LONG)(TotalSubmittedFrames() / m_NotificationFrames);
    if (m_LastOsWriteAbs == 0 && submittedAbs == 0) {
        // The OS's first release of the session. The unwrap below assumes a
        // prior accepted release and turns SetWritePacket(0) into delta 0 →
        // ahead 0 → "late" — which starves the pump forever: no accepted
        // release, no URB, no completion, no notification event, no further
        // writes from the engine (the handshake deadlock of the prime-less
        // pump; the old prime bootstrapped this by completing URBs that were
        // never there). Releasing packet k first means packets 0..k are in
        // the ring — the engine fills sequentially.
        m_LastOsWriteAbs = (ULONGLONG)PacketNumber + 1;
        if (InterlockedCompareExchange(&g_SwrSeen, 1, 0) == 0) {
            EmuNoteStatusValue(NULL, L"SwrFirst", PacketNumber);
        }
        EmuIsochRenderDataReady(m_Context);
        return STATUS_SUCCESS;
    }
    delta = (LONG)((ULONG)m_LastOsWriteAbs % m_NotificationCount);
    delta = (LONG)PacketNumber - delta;
    if (delta < 0) {
        delta += (LONG)m_NotificationCount;   // cyclic wrap: forward only
    }
    // ahead = how far past the consumed cursor the OS claims to have written.
    ahead = (LONG)(m_LastOsWriteAbs + (ULONGLONG)delta - submittedAbs);
    if (ahead < 1) {
        // The driver already consumed this packet: OS underrun.
        g_SwrLate++;
        InterlockedIncrement((volatile LONG*)&g_Underruns);
        return STATUS_DATA_LATE_ERROR;
    }
    if (ahead > EMU_ISOCH_SLOTS) {
        // Beyond the pipeline: the data would never be read in order.
        g_SwrOver++;
        return STATUS_DATA_OVERRUN;
    }
    m_LastOsWriteAbs += (ULONGLONG)delta;

    // Data arrival: the analog of the vendor's pump call from its KS ring
    // write (FUN_00013424). The pump arms URBs for everything released so
    // far; an empty pipeline starts here, re-anchored to the current frame.
    EmuIsochRenderDataReady(m_Context);

    // Flush the silence probe here: the packet-model engine talks to the
    // stream through Set/GetPacketCount and never calls the legacy
    // GetPosition, so this is the regular PASSIVE_LEVEL callback.
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        if (InterlockedCompareExchange(&g_FillProbeDirty, 0, 1) == 1) {
            EmuNoteStatusValue(NULL, L"FillNonZero", g_FillNonZero);
            EmuNoteStatusValue(NULL, L"FillSig0", g_FillSig0);
            EmuNoteStatusValue(NULL, L"FillSig1", g_FillSig1);
            EmuNoteStatusValue(NULL, L"HostNonZero", g_HostNonZero);
            EmuNoteStatusValue(NULL, L"HostSig0", g_HostSig0);
            EmuNoteStatusValue(NULL, L"HostSig1", g_HostSig1);
        EmuNoteStatusValue(NULL, L"ProbeSampled", g_ProbeSampled);
        }
        EmuNoteStatusValue(NULL, L"FillCalls", g_FillCalls);
        EmuNoteStatusValue(NULL, L"ArmTicks", g_ArmTicks);
EmuNoteStatusValue(NULL, L"Underruns", g_Underruns);
    }
    return STATUS_SUCCESS;
}

#pragma code_seg()
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::GetPacketCount(
    _Out_ ULONG* PacketCount
    )
{
    if (PacketCount == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (m_NotificationCount == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    // The OS fills the ring ahead of the CONSUMED cursor, so it must learn
    // the submitted count, not the completed one.
    *PacketCount = (ULONG)(TotalSubmittedFrames() / m_NotificationFrames);
    return STATUS_SUCCESS;
}

#pragma code_seg()
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::GetOutputStreamPresentationPosition(
    _Out_ KSAUDIO_PRESENTATION_POSITION* PresentationPosition
    )
{
    if (PresentationPosition == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (m_NotificationCount == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    PresentationPosition->u64PositionInBlocks = TotalCompletedFrames();
    PresentationPosition->u64QPCPosition =
        (UINT64)KeQueryPerformanceCounter(NULL).QuadPart;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::SetFormat(
    _In_ PKSDATAFORMAT DataFormat
    )
{
    PAGED_CODE();

    return EmuWaveValidateFormat(DataFormat);
}

#pragma code_seg("PAGE")
VOID CEmuWaveStream::StopAndReset()
{
    PAGED_CODE();

    ExAcquireFastMutex(&m_Context->StateLock);
    EmuIsochStopRender(m_Context);
    m_BaseFrames = 0;
    m_LastOsWriteAbs = 0;
    m_NextNotificationFrame = m_NotificationFrames;
    m_State = KSSTATE_STOP;
    ExReleaseFastMutex(&m_Context->StateLock);
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CEmuWaveStream::SetState(_In_ KSSTATE State)
{
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE();

    EmuNoteStatusValue(m_Context, L"StateEnter", (ULONG)State);
    ExAcquireFastMutex(&m_Context->StateLock);
    switch (State) {
    case KSSTATE_STOP:
        EmuIsochStopRender(m_Context);
        // The guaranteed PASSIVE flush of the silence probe: event-driven
        // clients can run a whole stream without touching the packet-model
        // callback at PASSIVE, and then nothing else reports what the fill
        // actually sent.
        if (InterlockedCompareExchange(&g_FillProbeDirty, 0, 1) == 1) {
            EmuNoteStatusValue(m_Context, L"FillNonZero", g_FillNonZero);
            EmuNoteStatusValue(m_Context, L"FillSig0", g_FillSig0);
            EmuNoteStatusValue(m_Context, L"FillSig1", g_FillSig1);
            EmuNoteStatusValue(m_Context, L"HostNonZero", g_HostNonZero);
            EmuNoteStatusValue(m_Context, L"HostSig0", g_HostSig0);
            EmuNoteStatusValue(m_Context, L"HostSig1", g_HostSig1);
            EmuNoteStatusValue(m_Context, L"ProbeSampled", g_ProbeSampled);
        }
        // Proves this flush ran at all: the probe keys being absent with
        // StopFlush present means the probe never sampled, not that the stop
        // path was skipped.
        EmuNoteStatusValue(m_Context, L"StopFlush", 0x10000001);
        EmuNoteStatusValue(m_Context, L"FillCalls", g_FillCalls);
        EmuNoteStatusValue(m_Context, L"ArmTicks", g_ArmTicks);
EmuNoteStatusValue(m_Context, L"Underruns", g_Underruns);
        // The release-path verdicts: SwrLate/SwrOver count the OS's releases
        // we refused; a stream that never filled the ring shows it here.
        EmuNoteStatusValue(m_Context, L"SwrLate", g_SwrLate);
        EmuNoteStatusValue(m_Context, L"SwrOver", g_SwrOver);
        // The fill-rejection forensics: reason, position, ring size.
        EmuNoteStatusValue(m_Context, L"FillRejectReason", g_FillRejectReason);
        EmuNoteStatusValue(m_Context, L"FillRejectPos",
                           (ULONG)g_FillRejectPos);
        EmuNoteStatusValue(m_Context, L"FillBufferBytes", g_FillBufferBytes);
        m_BaseFrames = 0;
        m_NextNotificationFrame = m_NotificationFrames;
        m_LastOsWriteAbs = 0;
        break;

    case KSSTATE_ACQUIRE:
        if (m_State == KSSTATE_STOP) {
            m_BaseFrames = 0;
            m_NextNotificationFrame = m_NotificationFrames;
        }
        break;

    case KSSTATE_PAUSE:
        if (m_State == KSSTATE_RUN) {
            // Stop first, then harvest the final completed count. Cancelled or
            // short OUT packets are deliberately absent from this position.
            EmuIsochStopRender(m_Context);
            m_BaseFrames += EmuIsochRenderCompletedFrames(m_Context);
        }
        break;

    case KSSTATE_RUN:
        if (m_State != KSSTATE_RUN) {
            if (m_Buffer == NULL || m_BufferBytes == 0) {
                status = STATUS_INVALID_DEVICE_STATE;
                break;
            }
            m_LastOsWriteAbs = 0;
            m_NextNotificationFrame = m_NotificationFrames;
            // The handshake verdict: the packet size the pump's data gate
            // and the release unwrap live by. Absent with a running stream
            // means the session never negotiated the packet model.
            EmuNoteStatusValue(m_Context, L"NotifyFrames",
                               (m_NotificationCount << 20) |
                               m_NotificationFrames);
            status = EmuIsochStartRender(
                m_Context, EMU_AUDIO_M4_RATE_HZ, m_BaseFrames,
                this, FillUsbPacket, FramesAvailable, UsbPacketCompleted);
            // Probe instrumentation: was the arm site reached at all, and with
            // which start verdict - the silence probe never sampled while the
            // stream demonstrably ran, so every step of its chain gets a wire
            // verdict now.
            EmuNoteStatusValue(m_Context, L"ProbeArm", (ULONG)status | 0x10000000);
            if (NT_SUCCESS(status)) {
                InterlockedIncrement((volatile LONG*)&g_ArmTicks);
                // Bootstrap the packet-model handshake: the engine paces its
                // ring writes on the notification events, and those fire only
                // from URB completions — while the pump arms no URB before
                // the OS releases data. Someone moves first; make it us.
                KickNotifications();
            }
        }
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    if (NT_SUCCESS(status)) {
        m_State = State;
    }
    ExReleaseFastMutex(&m_Context->StateLock);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "WaveRT SetState(%lu) failed, 0x%x\n",
                      (ULONG)State, status);
    }
    return status;
}

#pragma code_seg()
ULONG NTAPI CEmuWaveStream::FillUsbPacket(
    _Inout_ PVOID Context,
    _Out_writes_bytes_(Frames * 6) PUCHAR Destination,
    _In_ ULONG Frames,
    _In_ ULONGLONG LinearFramePosition
    )
{
    CEmuWaveStream* stream = (CEmuWaveStream*)Context;
    EMU_AUDIO_RING_SPAN spans[2];
    ULONG spanCount;
    ULONG hostFrameBytes = EMU_AUDIO_M4_CHANNELS * EMU_AUDIO_HOST_SAMPLE_BYTES;
    BOOLEAN passive = KeGetCurrentIrql() == PASSIVE_LEVEL;

    // Probe instrumentation: if this counter stays at zero while the stream
    // submits, the wire is fed by the prime-pass pattern, not by the client.
    // Runs in the ISO pump — PASSIVE from the write-packet path, DISPATCH
    // from the URB completion.
    InterlockedIncrement((volatile LONG*)&g_FillCalls);

    // One-shot direct registry writes: legal only at PASSIVE. If FillSeen
    // appears but the probe's ProbeLive does not, the divergence is inside
    // this function.
    if (passive && InterlockedCompareExchange(&g_FillSeen, 1, 0) == 0) {
        EmuNoteStatusValue(NULL, L"FillSeen", (ULONG)LinearFramePosition);
        EmuNoteStatusValue(NULL, L"FillSeenFrames", Frames);
        EmuNoteStatusValue(NULL, L"FillSeenBuffer", stream != NULL
                               ? stream->m_BufferBytes : 0);
    }

    if (stream == NULL || Destination == NULL || Frames == 0 ||
        stream->m_Buffer == NULL ||
        LinearFramePosition > (~(ULONGLONG)0) / hostFrameBytes ||
        Frames > stream->m_BufferBytes / hostFrameBytes) {
        g_FillRejectReason = 1;
        g_FillRejectPos = LinearFramePosition;
        g_FillBufferBytes = stream != NULL ? stream->m_BufferBytes : 0;
        // One-shot: the pump keeps calling with parameters this fill rejects.
        // The submit counters growing alongside this value would mean the
        // data gate never sees this function succeed.
        if (passive && InterlockedCompareExchange(&g_FillRejected, 1, 0) == 0) {
            EmuNoteStatusValue(NULL, L"FillRejected", Frames);
            EmuNoteStatusValue(NULL, L"FillRejectedPos",
                               (ULONG)LinearFramePosition);
            EmuNoteStatusValue(NULL, L"FillRejectedBuf", stream != NULL
                                   ? stream->m_BufferBytes : 0);
        }
        return 0;
    }
    if (!EmuAudioRingSpans(LinearFramePosition * hostFrameBytes,
                           stream->m_BufferBytes, hostFrameBytes, Frames,
                           spans, &spanCount)) {
        g_FillRejectReason = 2;
        g_FillRejectPos = LinearFramePosition;
        g_FillBufferBytes = stream->m_BufferBytes;
        return 0;
    }

    for (ULONG i = 0; i < spanCount; i++) {
        ULONG spanFrames = spans[i].Bytes / hostFrameBytes;
        if (!EmuAudioPack24(Destination,
                            stream->m_Buffer + spans[i].Offset,
                            spanFrames, EMU_AUDIO_M4_CHANNELS)) {
            g_FillRejectReason = 3;
            g_FillRejectPos = LinearFramePosition;
            g_FillBufferBytes = stream->m_BufferBytes;
            return 0;
        }
        Destination += spanFrames * EMU_AUDIO_M4_CHANNELS *
            EMU_AUDIO_USB_SAMPLE_BYTES;
    }

    // The old 6 ms write wait lived here; the pump's data gate replaced it.
    // The gate only lets a build through when the OS has already released
    // the packet (FramesAvailable), and the pump never waits — the vendor
    // pump has no wait either: no data, no URB, silent wire, re-anchor on
    // the next build after the write arrives.

    // Continuous probe: both what the OS left in the ring (host int 24-in-32)
    // and what the conversion sent out (packed 24). The engine parks the pin
    // between clients, so a one-shot-per-RUN sample kept describing the boot
    // idle stream instead of the tone - sample every fill instead (two
    // 1152-byte scans, nothing on the worker), keep the latest signatures in
    // the globals, and arm the flush once a second (48000 fills on the 1 ms
    // grid). The flush sites are PASSIVE-gated; the worker is PASSIVE.
    {
        PUCHAR packed = (PUCHAR)Destination -
            Frames * EMU_AUDIO_M4_CHANNELS * EMU_AUDIO_USB_SAMPLE_BYTES;
        PUCHAR host = stream->m_Buffer + spans[0].Offset;
        ULONG packedBytes = Frames * EMU_AUDIO_M4_CHANNELS *
            EMU_AUDIO_USB_SAMPLE_BYTES;
        ULONG hostBytes = spans[0].Bytes;   // span 0 only: wraps are irrelevant
        ULONG nzPacked = 0;
        ULONG nzHost = 0;

        for (ULONG b = 0; b < packedBytes; b++) {
            if (packed[b] != 0) {
                nzPacked++;
            }
        }
        for (ULONG b = 0; b < hostBytes; b++) {
            if (host[b] != 0) {
                nzHost++;
            }
        }
        g_FillNonZero = nzPacked;
        g_FillSig0 = (ULONG)packed[0] | ((ULONG)packed[1] << 8) |
                     ((ULONG)packed[2] << 16) | ((ULONG)packed[3] << 24);
        g_FillSig1 = (ULONG)packed[4] | ((ULONG)packed[5] << 8) |
                     ((ULONG)packed[6] << 16) | ((ULONG)packed[7] << 24);
        g_HostNonZero = nzHost;
        g_HostSig0 = (ULONG)host[0] | ((ULONG)host[1] << 8) |
                     ((ULONG)host[2] << 16) | ((ULONG)host[3] << 24);
        g_HostSig1 = (ULONG)host[4] | ((ULONG)host[5] << 8) |
                     ((ULONG)host[6] << 16) | ((ULONG)host[7] << 24);
        g_ProbeSampled = (ULONG)LinearFramePosition;
        if ((g_FillCalls % 2400) == 0) {
            InterlockedExchange(&g_FillProbeDirty, 1);
        }
    }
    return Frames;
}

#pragma code_seg()
ULONGLONG NTAPI CEmuWaveStream::FramesAvailable(
    _Inout_ PVOID Context,
    _In_ ULONGLONG LinearFramePosition
    )
{
    CEmuWaveStream* stream = (CEmuWaveStream*)Context;
    ULONGLONG released;

    if (stream == NULL || stream->m_NotificationCount == 0 ||
        stream->m_NotificationFrames == 0) {
        // No packet model: no write notification exists, so there is no gate
        // to consult — the pump fills freely, exactly as the probe stream
        // always has. The vendor analog has no such case (its KS ring always
        // knows its fill).
        return ~(ULONGLONG)0;
    }
    if (LinearFramePosition < stream->m_BaseFrames) {
        return 0;
    }
    // The OS releases whole notification packets (SetWritePacket); frames
    // past the last released packet boundary are not data yet. The pump
    // builds only when at least one URB of them has been released, and the
    // SetWritePacket overrun check (ahead > EMU_ISOCH_SLOTS) bounds how far
    // ahead that can be.
    released = stream->m_BaseFrames +
        stream->m_LastOsWriteAbs * stream->m_NotificationFrames;
    return released > LinearFramePosition ? released - LinearFramePosition : 0;
}

#pragma code_seg()
VOID NTAPI CEmuWaveStream::UsbPacketCompleted(
    _Inout_ PVOID Context,
    _In_ ULONGLONG CompletedFrames
    )
{
    CEmuWaveStream* stream = (CEmuWaveStream*)Context;
    ULONGLONG total;
    KIRQL oldIrql;
    BOOLEAN signal = FALSE;
    PLIST_ENTRY entry;

    if (stream == NULL || stream->m_NotificationFrames == 0) {
        return;
    }
    total = stream->m_BaseFrames + CompletedFrames;
    KeAcquireSpinLock(&stream->m_NotificationLock, &oldIrql);
    if (total >= stream->m_NextNotificationFrame) {
        signal = TRUE;
        do {
            stream->m_NextNotificationFrame += stream->m_NotificationFrames;
        } while (total >= stream->m_NextNotificationFrame &&
                 stream->m_NextNotificationFrame != 0);
    }
    if (signal) {
        for (entry = stream->m_NotificationList.Flink;
             entry != &stream->m_NotificationList; entry = entry->Flink) {
            PEMU_WAVE_NOTIFICATION notification = CONTAINING_RECORD(
                entry, EMU_WAVE_NOTIFICATION, Link);
            KeSetEvent(notification->Event, IO_NO_INCREMENT, FALSE);
        }
    }
    KeReleaseSpinLock(&stream->m_NotificationLock, oldIrql);
}

#pragma code_seg("PAGE")
VOID CEmuWaveStream::KickNotifications()
{
    PLIST_ENTRY entry;
    KIRQL oldIrql;

    PAGED_CODE();

    if (m_NotificationCount == 0 || IsListEmpty(&m_NotificationList)) {
        return;
    }
    KeAcquireSpinLock(&m_NotificationLock, &oldIrql);
    for (entry = m_NotificationList.Flink; entry != &m_NotificationList;
         entry = entry->Flink) {
        PEMU_WAVE_NOTIFICATION notification = CONTAINING_RECORD(
            entry, EMU_WAVE_NOTIFICATION, Link);
        KeSetEvent(notification->Event, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&m_NotificationLock, oldIrql);
    // One-shot wire verdict: the bootstrap fired, with which packet size.
    if (InterlockedCompareExchange(&g_NotifKick, 1, 0) == 0) {
        EmuNoteStatusValue(m_Context, L"NotifKick",
                           (m_NotificationCount << 20) | m_NotificationFrames);
    }
}

// --- Endpoint installation and teardown -------------------------------------

#pragma code_seg("PAGE")
static NTSTATUS EmuWaveInstallPort(
    _Inout_ PEMU_DEVICE_CONTEXT Context,
    _In_ PCWSTR ReferenceName,
    _In_ REFGUID PortClass,
    _In_ PUNKNOWN Miniport,
    _In_opt_ PIRP Irp,
    _In_opt_ PRESOURCELIST ResourceList,
    _Out_ PUNKNOWN* PortUnknown,
    _Out_ PBOOLEAN Registered
    )
{
    UNICODE_STRING reference;
    UNICODE_STRING symbolicLink;
    PPORT port = NULL;
    NTSTATUS status;

    PAGED_CODE();

    RtlZeroMemory(&symbolicLink, sizeof(symbolicLink));
    RtlInitUnicodeString(&reference, ReferenceName);
    *PortUnknown = NULL;
    *Registered = FALSE;
    status = IoRegisterDeviceInterface(Context->Pdo, &KSCATEGORY_AUDIO,
                                       &reference, &symbolicLink);
    if (NT_SUCCESS(status)) {
        status = PcNewPort(&port, PortClass);
    }
    if (NT_SUCCESS(status)) {
        status = port->Init(Context->Fdo, Irp, Miniport,
                            Context->AdapterUnknown, ResourceList);
    }
    if (NT_SUCCESS(status)) {
        status = port->QueryInterface(IID_IUnknown, (PVOID*)PortUnknown);
    }
    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(Context->Fdo,
                                     const_cast<PWSTR>(ReferenceName), port);
        if (NT_SUCCESS(status)) {
            *Registered = TRUE;
        }
    }
    if (symbolicLink.Buffer != NULL) {
        RtlFreeUnicodeString(&symbolicLink);
    }
    if (port != NULL) {
        port->Release();
    }
    if (!NT_SUCCESS(status) && *PortUnknown != NULL) {
        (*PortUnknown)->Release();
        *PortUnknown = NULL;
    }
    return status;
}

#pragma code_seg("PAGE")
static VOID EmuWaveUnregisterPort(
    _In_ PEMU_DEVICE_CONTEXT Context,
    _Inout_ PUNKNOWN* PortUnknown,
    _Inout_ PBOOLEAN Registered
    )
{
    PAGED_CODE();

    if (*PortUnknown != NULL) {
        if (*Registered) {
            PUNREGISTERSUBDEVICE unregister = NULL;
            NTSTATUS status = (*PortUnknown)->QueryInterface(
                IID_IUnregisterSubdevice, (PVOID*)&unregister);
            if (NT_SUCCESS(status)) {
                status = unregister->UnregisterSubdevice(Context->Fdo,
                                                          *PortUnknown);
                unregister->Release();
            }
            if (!NT_SUCCESS(status)) {
                EMU_LOG_WARN(STR_MODULENAME "unregister audio port failed, 0x%x\n",
                             status);
            }
        }
        (*PortUnknown)->Release();
        *PortUnknown = NULL;
    }
    *Registered = FALSE;
}

#pragma code_seg("PAGE")
NTSTATUS EmuWaveStart(
    _Inout_ PEMU_DEVICE_CONTEXT Context,
    _In_opt_ PIRP Irp,
    _In_opt_ PRESOURCELIST ResourceList
    )
{
    CEmuAdapter* adapter;
    CEmuWaveMiniport* wave = NULL;
    CEmuTopologyMiniport* topology = NULL;
    NTSTATUS status;

    PAGED_CODE();

    if (Context->AdapterUnknown != NULL || Context->WavePort != NULL ||
        Context->TopologyPort != NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    adapter = new (POOL_FLAG_NON_PAGED, EMU_POOLTAG) CEmuAdapter(NULL, Context);
    if (adapter == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    adapter->AddRef();
    Context->AdapterUnknown = PUNKNOWN(PADAPTERPOWERMANAGEMENT(adapter));

    wave = new (POOL_FLAG_NON_PAGED, EMU_POOLTAG) CEmuWaveMiniport(NULL, Context);
    if (wave != NULL) {
        wave->AddRef();
    }
    topology = new (POOL_FLAG_NON_PAGED, EMU_POOLTAG) CEmuTopologyMiniport(NULL);
    if (topology != NULL) {
        topology->AddRef();
    }
    if (wave == NULL || topology == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Failure;
    }

    status = EmuWaveInstallPort(Context, L"WaveEmuRender", CLSID_PortWaveRT,
                                PUNKNOWN(PMINIPORTWAVERT(wave)), Irp, ResourceList,
                                &Context->WavePort, &Context->WavePortRegistered);
    if (!NT_SUCCESS(status)) {
        goto Failure;
    }
    status = EmuWaveInstallPort(Context, L"TopologyEmuRender", CLSID_PortTopology,
                                PUNKNOWN(PMINIPORTTOPOLOGY(topology)), Irp,
                                ResourceList, &Context->TopologyPort,
                                &Context->TopologyPortRegistered);
    if (!NT_SUCCESS(status)) {
        goto Failure;
    }
    status = PcRegisterPhysicalConnection(Context->Fdo, Context->WavePort,
                                          EMU_WAVE_PIN_SOURCE,
                                          Context->TopologyPort,
                                          EMU_TOPO_PIN_SOURCE);
    if (!NT_SUCCESS(status)) {
        goto Failure;
    }
    Context->PhysicalConnectionRegistered = TRUE;
    wave->Release();
    topology->Release();
    EMU_LOG_TERSE(STR_MODULENAME "WaveRT render endpoint registered\n");
    return STATUS_SUCCESS;

Failure:
    if (wave != NULL) {
        wave->Release();
    }
    if (topology != NULL) {
        topology->Release();
    }
    EMU_LOG_ERROR(STR_MODULENAME "WaveRT endpoint setup failed, 0x%x\n", status);
    EmuWaveStop(Context);
    return status;
}

#pragma code_seg("PAGE")
VOID EmuWaveStop(_Inout_ PEMU_DEVICE_CONTEXT Context)
{
    PAGED_CODE();

    ExAcquireFastMutex(&Context->StateLock);
    EmuIsochStopRender(Context);
    ExReleaseFastMutex(&Context->StateLock);

    if (Context->PhysicalConnectionRegistered && Context->WavePort != NULL &&
        Context->TopologyPort != NULL) {
        PUNREGISTERPHYSICALCONNECTION unregister = NULL;
        NTSTATUS status = Context->WavePort->QueryInterface(
            IID_IUnregisterPhysicalConnection, (PVOID*)&unregister);
        if (NT_SUCCESS(status)) {
            status = unregister->UnregisterPhysicalConnection(
                Context->Fdo, Context->WavePort, EMU_WAVE_PIN_SOURCE,
                Context->TopologyPort, EMU_TOPO_PIN_SOURCE);
            unregister->Release();
        }
        if (!NT_SUCCESS(status)) {
            EMU_LOG_WARN(STR_MODULENAME "unregister physical connection failed, "
                         "0x%x\n", status);
        }
        Context->PhysicalConnectionRegistered = FALSE;
    }

    EmuWaveUnregisterPort(Context, &Context->WavePort,
                          &Context->WavePortRegistered);
    EmuWaveUnregisterPort(Context, &Context->TopologyPort,
                          &Context->TopologyPortRegistered);
    if (Context->AdapterUnknown != NULL) {
        Context->AdapterUnknown->Release();
        Context->AdapterUnknown = NULL;
    }
}

#pragma code_seg()
