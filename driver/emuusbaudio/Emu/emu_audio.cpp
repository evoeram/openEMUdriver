// EmuUSBAudio — WaveRT audio format and position helpers (M4 foundation).
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "emu_audio.h"

ULONG
EmuAudioHostFrameBytes(
    _In_ ULONG Channels
    )
{
    if (Channels == 0 || Channels > ((ULONG)~0UL) / EMU_AUDIO_HOST_SAMPLE_BYTES) {
        return 0;
    }
    return Channels * EMU_AUDIO_HOST_SAMPLE_BYTES;
}

ULONG
EmuAudioUsbFrameBytes(
    _In_ ULONG Channels
    )
{
    if (Channels == 0 || Channels > ((ULONG)~0UL) / EMU_AUDIO_USB_SAMPLE_BYTES) {
        return 0;
    }
    return Channels * EMU_AUDIO_USB_SAMPLE_BYTES;
}

static BOOLEAN
EmuAudioSampleCount(
    _In_ SIZE_T Frames,
    _In_ ULONG Channels,
    _Out_ PSIZE_T Samples
    )
{
    SIZE_T samples;

    if (Samples == NULL || Channels == 0) {
        return FALSE;
    }
    if (Frames != 0 && (SIZE_T)Channels > ((SIZE_T)-1) / Frames) {
        return FALSE;
    }
    samples = Frames * (SIZE_T)Channels;
    if (samples > ((SIZE_T)-1) / EMU_AUDIO_HOST_SAMPLE_BYTES) {
        return FALSE;
    }
    *Samples = samples;
    return TRUE;
}

BOOLEAN
EmuAudioPack24(
    _Out_writes_bytes_(Frames * Channels * EMU_AUDIO_USB_SAMPLE_BYTES)
        PUCHAR Destination,
    _In_reads_bytes_(Frames * Channels * EMU_AUDIO_HOST_SAMPLE_BYTES)
        const UCHAR* Source,
    _In_ SIZE_T Frames,
    _In_ ULONG Channels
    )
{
    SIZE_T samples;

    if (!EmuAudioSampleCount(Frames, Channels, &samples)) {
        return FALSE;
    }
    if (Frames == 0) {
        return TRUE;
    }
    if (Destination == NULL || Source == NULL) {
        return FALSE;
    }

    for (SIZE_T i = 0; i < samples; i++) {
        // WAVEFORMATEXTENSIBLE puts 24 valid bits in bits 8..31. Dropping the
        // low container byte leaves exactly the packed little-endian USB sample.
        Destination[0] = Source[1];
        Destination[1] = Source[2];
        Destination[2] = Source[3];
        Destination += EMU_AUDIO_USB_SAMPLE_BYTES;
        Source += EMU_AUDIO_HOST_SAMPLE_BYTES;
    }
    return TRUE;
}

BOOLEAN
EmuAudioUnpack24(
    _Out_writes_bytes_(Frames * Channels * EMU_AUDIO_HOST_SAMPLE_BYTES)
        PUCHAR Destination,
    _In_reads_bytes_(Frames * Channels * EMU_AUDIO_USB_SAMPLE_BYTES)
        const UCHAR* Source,
    _In_ SIZE_T Frames,
    _In_ ULONG Channels
    )
{
    SIZE_T samples;

    if (!EmuAudioSampleCount(Frames, Channels, &samples)) {
        return FALSE;
    }
    if (Frames == 0) {
        return TRUE;
    }
    if (Destination == NULL || Source == NULL) {
        return FALSE;
    }

    for (SIZE_T i = 0; i < samples; i++) {
        Destination[0] = 0;
        Destination[1] = Source[0];
        Destination[2] = Source[1];
        Destination[3] = Source[2];
        Destination += EMU_AUDIO_HOST_SAMPLE_BYTES;
        Source += EMU_AUDIO_USB_SAMPLE_BYTES;
    }
    return TRUE;
}

BOOLEAN
EmuAudioRingSpans(
    _In_ ULONGLONG LinearBytePosition,
    _In_ ULONG BufferBytes,
    _In_ ULONG FrameBytes,
    _In_ ULONG Frames,
    _Out_writes_(2) PEMU_AUDIO_RING_SPAN Spans,
    _Out_ PULONG SpanCount
    )
{
    EMU_AUDIO_RING_SPAN local[2] = {{0, 0}, {0, 0}};
    ULONG requestBytes;
    ULONG offset;
    ULONG count;

    if (Spans == NULL || SpanCount == NULL || BufferBytes == 0 ||
        FrameBytes == 0 || (BufferBytes % FrameBytes) != 0 ||
        (LinearBytePosition % FrameBytes) != 0 ||
        Frames > ((ULONG)~0UL) / FrameBytes) {
        return FALSE;
    }

    requestBytes = Frames * FrameBytes;
    offset = (ULONG)(LinearBytePosition % BufferBytes);
    if (requestBytes != 0) {
        local[0].Offset = offset;
        local[0].Bytes = requestBytes;
        if (local[0].Bytes > BufferBytes - offset) {
            local[0].Bytes = BufferBytes - offset;
        }
        local[1].Offset = 0;
        local[1].Bytes = requestBytes - local[0].Bytes;
        count = local[1].Bytes != 0 ? 2 : 1;
    } else {
        count = 0;
    }

    Spans[0] = local[0];
    Spans[1] = local[1];
    *SpanCount = count;
    return TRUE;
}

BOOLEAN
EmuAudioPositions(
    _In_ ULONGLONG CompletedFrames,
    _In_ ULONG BufferBytes,
    _In_ ULONG FrameBytes,
    _Out_ PULONGLONG LinearBytePosition,
    _Out_ PULONG RingBytePosition
    )
{
    ULONGLONG linear;
    ULONG ring;

    if (LinearBytePosition == NULL || RingBytePosition == NULL ||
        BufferBytes == 0 || FrameBytes == 0 ||
        (BufferBytes % FrameBytes) != 0 ||
        CompletedFrames > (~(ULONGLONG)0) / FrameBytes) {
        return FALSE;
    }

    linear = CompletedFrames * FrameBytes;
    ring = (ULONG)(linear % BufferBytes);
    *LinearBytePosition = linear;
    *RingBytePosition = ring;
    return TRUE;
}
