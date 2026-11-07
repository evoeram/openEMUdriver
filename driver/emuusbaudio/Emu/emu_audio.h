// EmuUSBAudio — WaveRT audio format and position helpers (M4 foundation).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// This header deliberately has no PortCls or USB dependency. The same source is
// compiled for the kernel and for driver/utest.sh, so byte layout, wrap and
// overflow rules are pinned before the streaming code calls them.
#ifndef _EMU_AUDIO_H_
#define _EMU_AUDIO_H_

#ifdef EMU_KERNEL
#include <ntddk.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// M4 exposes stereo 48 kHz, 24 valid bits left-aligned in 32-bit containers.
// USB carries the same samples packed into three bytes, little-endian.
#define EMU_AUDIO_M4_RATE_HZ          48000
#define EMU_AUDIO_M4_CHANNELS         2
#define EMU_AUDIO_HOST_SAMPLE_BYTES   4
#define EMU_AUDIO_USB_SAMPLE_BYTES    3
/* The pin data range must advertise the VALID bit depth: PortCls's generic
 * data intersection derives the container from it, and a 32/32 range yields
 * a plain 32-bit integer WAVE_FORMAT_PCM the audio engine cannot convert. */
#define EMU_AUDIO_M4_VALID_BITS       24

ULONG
EmuAudioHostFrameBytes(
    _In_ ULONG Channels
    );

ULONG
EmuAudioUsbFrameBytes(
    _In_ ULONG Channels
    );

// Convert contiguous whole frames. Zero frames is a successful no-op, including
// with NULL pointers. Non-empty conversions require non-NULL, non-overlapping
// buffers. FALSE rejects invalid arguments and size arithmetic overflow; no
// partial conversion is attempted in that case.
BOOLEAN
EmuAudioPack24(
    _Out_writes_bytes_(Frames * Channels * EMU_AUDIO_USB_SAMPLE_BYTES)
        PUCHAR Destination,
    _In_reads_bytes_(Frames * Channels * EMU_AUDIO_HOST_SAMPLE_BYTES)
        const UCHAR* Source,
    _In_ SIZE_T Frames,
    _In_ ULONG Channels
    );

BOOLEAN
EmuAudioUnpack24(
    _Out_writes_bytes_(Frames * Channels * EMU_AUDIO_HOST_SAMPLE_BYTES)
        PUCHAR Destination,
    _In_reads_bytes_(Frames * Channels * EMU_AUDIO_USB_SAMPLE_BYTES)
        const UCHAR* Source,
    _In_ SIZE_T Frames,
    _In_ ULONG Channels
    );

// At most two contiguous spans cover a request in a circular buffer. The linear
// position and all sizes are bytes; BufferBytes and LinearBytePosition must be
// whole-frame aligned. Requests larger than the ring are legal and still produce
// at most two spans. A zero-frame request succeeds with SpanCount == 0.
typedef struct _EMU_AUDIO_RING_SPAN {
    ULONG Offset;
    ULONG Bytes;
} EMU_AUDIO_RING_SPAN, *PEMU_AUDIO_RING_SPAN;

BOOLEAN
EmuAudioRingSpans(
    _In_ ULONGLONG LinearBytePosition,
    _In_ ULONG BufferBytes,
    _In_ ULONG FrameBytes,
    _In_ ULONG Frames,
    _Out_writes_(2) PEMU_AUDIO_RING_SPAN Spans,
    _Out_ PULONG SpanCount
    );

// Translate a 64-bit completed-audio-frame count into both the linear byte
// position and the WaveRT ring offset. The caller supplies the host frame size;
// FrameBytes is never inferred from the USB wire format.
BOOLEAN
EmuAudioPositions(
    _In_ ULONGLONG CompletedFrames,
    _In_ ULONG BufferBytes,
    _In_ ULONG FrameBytes,
    _Out_ PULONGLONG LinearBytePosition,
    _Out_ PULONG RingBytePosition
    );

#endif // _EMU_AUDIO_H_
