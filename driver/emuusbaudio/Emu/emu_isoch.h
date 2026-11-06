// EmuUSBAudio — isochronous stream engine, M3 stage B (test slice).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The first vertical slice of kernel iso streaming, built on the alternates the
// select-interface path now really selects: a fixed grid of one packet per USB
// (milli)frame, EMU_ISOCH_SLOTS URBs in flight per direction, one packet per
// URB, each re-armed by a per-direction system thread after its completion
// event. Scheduling facts come from the pipe, not from assumptions: the period
// is derived from USBD_PIPE_INFORMATION.Interval, and the nominal packet is
// derived from the rate the caller asked for — this slice accepts only what it
// can schedule exactly (integer frames per packet), which on the 0404 grid is
// the /1000 Hz rates: 48000 in the test contract, 44100 waits for the M5
// feedback engine.
//
// The pure scheduling helpers at the top are compiled twice (kernel image and
// driver/utest.sh unit "isoch"), so the numbers the engine lives by are the
// numbers the test checked. The engine below them is kernel-only.
#ifndef _EMU_ISOCH_H_
#define _EMU_ISOCH_H_

#ifdef EMU_KERNEL
#include <ntddk.h>
#include <usb.h>           // PURB, USBD_PIPE_HANDLE for the slot structs
#else
#ifndef WIN32_NO_STATUS
#define WIN32_NO_STATUS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef WIN32_NO_STATUS
#undef WIN32_NO_STATUS
#endif
#ifndef NT_SUCCESS
typedef LONG NTSTATUS;
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif
#include <ntstatus.h>
#endif

#include "emu_desc.h"      // EMU_STREAM_RENDER / EMU_STREAM_CAPTURE
#include "emu_ioctl.h"     // EMU_TEST_STREAM_STATS, the stats reply layout

// The engine API below takes the owning context by pointer only, and emu.h —
// which embeds these structs — includes this header before defining the
// context. A forward declaration breaks the cycle; nothing here dereferences.
typedef struct _EMU_DEVICE_CONTEXT EMU_DEVICE_CONTEXT, *PEMU_DEVICE_CONTEXT;

// URBs in flight per direction. The plan's first slice names 6-8; 8 gives a
// full millisecond of slack per slot at the 1 ms grid and matches the queue
// depth v1 proved on this hardware.
#define EMU_ISOCH_SLOTS              8

// StartFrame of the first packet: this many frames past the current frame
// number, the reserve v1 used (the submission itself takes a few frames).
#define EMU_ISOCH_FRAME_MARGIN       8

// Frames of headroom in an IN slot: the device may deliver up to the pipe's
// maximum, and the harvest reads what actually arrived.
#define EMU_ISOCH_SLOT_HEADROOM      2

// The engine's owns-one-direction state. Lives inside EMU_DEVICE_CONTEXT, so
// everything here is zeroed by IoCreateDevice and dies with the instance.
#ifdef EMU_KERNEL

typedef struct _EMU_ISOCH_SLOT {
    PIRP    Irp;            // allocated once, resubmitted per packet
    PURB    Urb;            // USBD_IsochUrbAllocate, one packet
    KEVENT  Done;           // signalled by the URB completion routine
    ULONG   NextFrame;      // StartFrame for the next submission
    BOOLEAN Submitted;      // whether the last submit is possibly in flight
} EMU_ISOCH_SLOT, *PEMU_ISOCH_SLOT;

typedef NTSTATUS (NTAPI *PEMU_ISOCH_FILL_RENDER)(
    _Inout_ PVOID Context,
    _Out_ PUCHAR Destination,
    _In_ ULONG Frames,
    _In_ ULONGLONG LinearFramePosition
    );

typedef VOID (NTAPI *PEMU_ISOCH_RENDER_COMPLETED)(
    _Inout_ PVOID Context,
    _In_ ULONGLONG CompletedFrames
    );

typedef struct _EMU_ISOCH_STREAM {
    BOOLEAN          Active;          // set once, cleared by the stop path
    LONG             Quit;            // interlocked stop flag for the thread
    PVOID            Owner;           // the owning EMU_DEVICE_CONTEXT
    ULONG            Direction;       // EMU_STREAM_RENDER / EMU_STREAM_CAPTURE
    PVOID            ThreadObject;    // referenced thread object, waited on stop
    HANDLE           ThreadHandle;    // fallback until ObReferenceObject succeeds
    // Pipe facts, read out of the submitted select-interface URB's interface
    // information once at start and treated as read-only afterwards.
    USBD_PIPE_HANDLE Pipe;            // the data pipe (OUT 0x01 / IN 0x82)
    USBD_PIPE_HANDLE FeedbackPipe;    // render only: 0x81, parked for M5
    ULONG            MaxPacketSize;
    ULONG            PipeInterval;    // raw USBD_PIPE_INFORMATION.Interval
    ULONG            PeriodMs;        // derived; 1 on the 0404 grid
    ULONG            NominalFrames;   // frames per packet at this rate
    ULONG            RateHz;          // the rate the stream was armed at
    ULONG            BytesPerFrame;   // channels * subframe size (2*3 on 0404)
    ULONG            SlotBytes;       // per-slot buffer size
    PUCHAR           Buffer;          // nonpaged, EMU_ISOCH_SLOTS * SlotBytes
    EMU_ISOCH_SLOT   Slots[EMU_ISOCH_SLOTS];
    // WaveRT render callbacks. The worker fills from the host ring before every
    // submit, including the prime pass, and reports only successful completions.
    PVOID                       CallbackContext;
    PEMU_ISOCH_FILL_RENDER      FillRender;
    PEMU_ISOCH_RENDER_COMPLETED RenderCompleted;
    ULONGLONG                   BaseFrames;
    volatile LONGLONG           SubmittedFrames;
    volatile LONGLONG           CompletedFrames;
    // Counters, all interlocked: the worker thread writes, the stats IOCTL
    // reads, and neither holds a lock against the other.
    volatile LONG    Submits;
    volatile LONG    Completes;
    volatile LONG    UrbFailures;
    volatile LONG    IsoPacketErrors;
    volatile LONG    ShortPackets;    // IN below nominal; OUT below requested
    volatile LONG    Bytes;
    volatile LONG    LastUrbStatus;
    volatile LONG    LastPacketStatus;
} EMU_ISOCH_STREAM, *PEMU_ISOCH_STREAM;

#endif // EMU_KERNEL

// --- pure scheduling helpers (compiled twice, tested user-mode) --------------

// USBD_PIPE_INFORMATION.Interval to the packet period in milliseconds.
//
// The interval's units differ per bus speed: full-speed isochronous counts in
// whole frames (bInterval, 1 ms each), high-speed in 2^(bInterval-1)
// microframes (125 us units). USBD_PIPE_INFORMATION does not say which bus the
// pipe sits on, so the rule prefers the high-speed reading whenever the value
// is a whole multiple of 8 (1 ms = 8 units on the 0404's proven grid) and
// falls back to the full-speed reading otherwise. A full-speed pipe with
// bInterval 8 would be misread as high-speed 1 ms - a pathological descriptor,
// accepted consciously and called out by the TestPipe breadcrumb when the
// hardware verdict disagrees.
BOOLEAN
EmuIsochPeriodMs(
    _In_ ULONG Interval,
    _Out_ PULONG PeriodMs
    );

// Frames per packet at RateHz on a grid of one packet every PeriodMs
// milliseconds. This slice streams only what divides exactly: 48000 Hz on the
// 1 ms grid is 48 frames, while 44100 would need the 44/45 alternation that is
// the M5 feedback engine's business. FALSE means "not schedulable by this
// slice", not "the device cannot do the rate".
BOOLEAN
EmuIsochNominalFramesExact(
    _In_ ULONG RateHz,
    _In_ ULONG PeriodMs,
    _Out_ PULONG NominalFrames
    );

// Per-slot buffer size: the pipe's maximum (an IN slot must hold whatever the
// device actually sends) but never less than the nominal packet plus headroom
// frames, so a 48 kHz OUT slot is 50 frames * bytes-per-frame on the 0404.
ULONG
EmuIsochSlotBytes(
    _In_ ULONG NominalFrames,
    _In_ ULONG BytesPerFrame,
    _In_ ULONG MaxPacketSize
    );

#ifdef EMU_KERNEL

// --- engine (kernel only) ----------------------------------------------------
//
// Start/stop are PASSIVE_LEVEL operations and serialize on the caller's
// (StateLock): the start selects alternates and arms threads, the stop aborts
// pipes and drains. The worker threads touch only stream-owned resources, so a
// stop that holds the lock while waiting for them cannot deadlock.

// Arm both directions for the M3 test contract: 48 kHz stereo, the alternates
// EmuCapsFindAlt names, rate pushed through the existing SET path first.
// Either direction failing stops what already started; stats describe whatever
// is left running.
NTSTATUS
EmuIsochStartStreams(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ ULONG                  RateHz
    );

// Start only the WaveRT render direction. StateLock must already be held.
// BaseFrames is the paused stream's preserved host-ring position; the fill
// callback receives monotonically increasing positions beginning there.
NTSTATUS
EmuIsochStartRender(
    _Inout_ PEMU_DEVICE_CONTEXT       Ctx,
    _In_ ULONG                        RateHz,
    _In_ ULONGLONG                    BaseFrames,
    _Inout_ PVOID                     CallbackContext,
    _In_ PEMU_ISOCH_FILL_RENDER       FillRender,
    _In_ PEMU_ISOCH_RENDER_COMPLETED  RenderCompleted
    );

// Stop render and wait for its worker. The completed count remains readable
// after the stop so RUN -> PAUSE can preserve it in the WaveRT base position.
VOID
EmuIsochStopRender(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    );

ULONGLONG
EmuIsochRenderCompletedFrames(
    _In_ PEMU_DEVICE_CONTEXT Ctx
    );

// Stop both directions (idempotent; safe on a never-started instance).
VOID
EmuIsochStopAll(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    );

// Fill the test stats reply from both engines. Counters are read with
// interlocked loads; the caller holds the state lock, which makes the pipe
// facts a stable read rather than a race.
VOID
EmuIsochFillStats(
    _Out_ PEMU_TEST_STREAM_STATS Out,
    _In_ PEMU_DEVICE_CONTEXT     Ctx
    );

#endif // EMU_KERNEL

#endif // _EMU_ISOCH_H_
