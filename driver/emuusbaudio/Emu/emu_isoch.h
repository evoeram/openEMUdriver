// EmuUSBAudio — isochronous stream engine, M3 stage B (vendor pump model).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// One direction = one pump, after the disassembly of emusba10.sys
// (FUN_000148A8, docs/ISOCH_PUMP.md): a bounded loop that builds and submits
// URBs while a free slot exists and — for render — the data behind the URB is
// already released by the OS. Nothing waits inside: no thread, no timers, no
// events. The pump is called from the vendor's three event sites — data
// arrival (the WaveRT SetWritePacket path, the analog of the vendor's KS ring
// write), URB completion (DPC, where the re-arm happens with zero gap), and
// start/resume. One global StartFrame cursor per stream advances +1 per URB
// and is re-anchored to the bus on EVERY build (cursor <= now -> now + 3, the
// vendor's invariant at 0x4DBE), so a stall of any length re-anchors the
// schedule to the future instead of leaving the slots firing into the past.
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

// URBs in flight per direction: the vendor trace and the disassembly of
// emusba10 agree on eight (the schedule covers 8 ms ahead; the start anchor
// is the current frame + 3). Each URB carries TWO 24-frame packets inside one
// millisecond frame (microframes 0 and 4 of the bInterval=3 endpoint).
#define EMU_ISOCH_SLOTS              8
#define EMU_ISOCH_PKTS_PER_URB       2

// StartFrame of the first packet: the vendor sync anchor (GET Id 0x10002
// returns GET_CURRENT_FRAME_NUMBER + 3; the RUN transition writes the same
// into its schedule).
#define EMU_ISOCH_FRAME_MARGIN       3

// Feedback endpoint reader: the vendor keeps TWO iso IN URBs in flight and
// spaces consecutive StartFrames by exactly +32 (the trace's feedback delta).
#define EMU_ISOCH_FB_SLOTS           2
#define EMU_ISOCH_FB_STEP            32

// Inline completions (IoCallDriver returning without STATUS_PENDING) re-enter
// the pump on the same stack; a run this long means a deterministic rejection
// or instant completions of past-scheduled URBs, and the pump leaves the rest
// to the next event instead of spinning at DISPATCH.
#define EMU_ISOCH_SYNC_SPIN_LIMIT    16

// Frames of headroom in an IN slot: the device may deliver up to the pipe's
// maximum, and the harvest reads what actually arrived.
#define EMU_ISOCH_SLOT_HEADROOM      2

// Slot lifecycle for the pump: FREE means the pump may arm it, ARMED means
// an IRP is in flight. The ARMED -> FREE interlocked exchange decides which
// of the competing reapers (the completion routine, the pump after a
// synchronous completion) owns the harvest.
#define EMU_ISOCH_SLOT_FREE          0
#define EMU_ISOCH_SLOT_ARMED         1

// The engine's owns-one-direction state. Lives inside EMU_DEVICE_CONTEXT, so
// everything here is zeroed by IoCreateDevice and dies with the instance.
#ifdef EMU_KERNEL

typedef struct _EMU_ISOCH_STREAM EMU_ISOCH_STREAM, *PEMU_ISOCH_STREAM;

typedef struct _EMU_ISOCH_SLOT {
    PIRP    Irp;            // allocated once, resubmitted per packet
    PURB    Urb;            // USBD_IsochUrbAllocate, two packets
    PEMU_ISOCH_STREAM Stream; // back-pointer for the completion routine
    volatile LONG State;    // EMU_ISOCH_SLOT_FREE / EMU_ISOCH_SLOT_ARMED
} EMU_ISOCH_SLOT, *PEMU_ISOCH_SLOT;

typedef struct _EMU_ISOCH_FB_SLOT {
    PIRP    Irp;
    PURB    Urb;            // USBD_IsochUrbAllocate, one 4-byte packet
    PUCHAR  Buffer;         // nonpaged, 8 bytes
    PEMU_ISOCH_STREAM Stream;
    volatile LONG State;    // EMU_ISOCH_SLOT_FREE / EMU_ISOCH_SLOT_ARMED
} EMU_ISOCH_FB_SLOT, *PEMU_ISOCH_FB_SLOT;

// Fill one packet's worth of frames into the USB transfer buffer, packed.
// Returns the number of frames actually written (may be short on an underrun
// — the URB then goes out with the actual length, as the vendor's builder
// does), 0 when nothing went in and the URB must not be submitted.
typedef ULONG (NTAPI *PEMU_ISOCH_FILL_RENDER)(
    _Inout_ PVOID Context,
    _Out_ PUCHAR Destination,
    _In_ ULONG Frames,
    _In_ ULONGLONG LinearFramePosition
    );

// How many frames past LinearFramePosition the client has released for
// consumption. The pump's data gate: below one URB of released data the pump
// stops, and the wire stays silent until the next write packet (the vendor's
// fill < need break at 0x4A60). DPC-safe: reads counters only.
typedef ULONGLONG (NTAPI *PEMU_ISOCH_RENDER_AVAILABLE)(
    _Inout_ PVOID Context,
    _In_ ULONGLONG LinearFramePosition
    );

typedef VOID (NTAPI *PEMU_ISOCH_RENDER_COMPLETED)(
    _Inout_ PVOID Context,
    _In_ ULONGLONG CompletedFrames
    );

struct _EMU_ISOCH_STREAM {
    BOOLEAN          Active;          // set under PumpLock, cleared on retire
    volatile LONG    Quit;            // retire flag: no pump may arm anymore
    PVOID            Owner;           // the owning EMU_DEVICE_CONTEXT
    ULONG            Direction;       // EMU_STREAM_RENDER / EMU_STREAM_CAPTURE
    // Pipe facts, read out of the submitted select-interface URB's interface
    // information once at start and treated as read-only afterwards.
    USBD_PIPE_HANDLE Pipe;            // the data pipe (OUT 0x01 / IN 0x82)
    USBD_PIPE_HANDLE FeedbackPipe;    // render only: 0x81
    ULONG            MaxPacketSize;
    ULONG            PipeInterval;    // raw USBD_PIPE_INFORMATION.Interval
    ULONG            PeriodMs;        // legacy breadcrumb field; 1
    ULONG            PeriodUs;        // packet period in microseconds (HS grid)
    ULONG            FramesPerPacket; // frames in one wire packet
    ULONG            NominalFrames;   // frames per packet at this rate
    ULONG            RateHz;          // the rate the stream was armed at
    ULONG            BytesPerFrame;   // channels * subframe size (2*3 on 0404)
    ULONG            SlotBytes;       // per-slot buffer size
    // The vendor pump model: one schedule cursor per stream, re-anchored to
    // the bus on every build. The re-arm lives in the URB completion (DPC);
    // Pending counts armed URBs and drains to zero on stop.
    KSPIN_LOCK       PumpLock;        // serializes the pump, arming and retire
    KEVENT           Idle;            // set when Pending drains to zero
    volatile LONG    Pending;         // armed URBs, data + feedback
    volatile ULONG   CursorFrame;     // data StartFrame cursor, +1 per URB
    volatile ULONG   FbCursorFrame;   // feedback StartFrame cursor, +32 per URB
    volatile LONG    Starved;         // set while the data gate blocks
    volatile LONG    Underruns;       // gate-blocked pump passes since start
    volatile LONG    Pumping;         // single-flight guard for the pump
    volatile LONG    FbRearmDepth;    // bounds inline-completion re-arm chains
    // Freeze forensics, flushed to the registry at stop: frame-query
    // outcomes, the anchor's view of the bus, the pump's exit reasons.
    volatile LONG    FqInline;        // real queries answered
    volatile LONG    FqFloat;         // queries still pending after the wait
    volatile LONG    FqFail;          // queries failed
    volatile LONG    FqBusy;          // queries skipped (previous in flight)
    volatile LONG    AnchorFires;     // cursor <= now re-anchors
    volatile ULONG   NowLast;         // last anchor source value
    volatile ULONG   CursorLast;      // data cursor after the last arm
    volatile LONG    PumpQuitExit;    // pump exits: retired
    volatile LONG    PumpPoolExit;    // pump exits: pool exhausted
    volatile LONG    PumpGateExit;    // pump exits: data gate blocked
    volatile LONG    PumpBuildExit;   // pump exits: build produced nothing
    // Frame number source for the anchor: a dedicated GET_CURRENT_FRAME_NUMBER
    // IRP the stack answers inline when possible, else the interrupt-time
    // mapping calibrated at start and refreshed by every successful query.
    PIRP             FrameIrp;
    PURB             FrameUrb;
    KEVENT           FrameEvent;
    volatile LONG    FrameBusy;
    ULONGLONG        PaceOrigin;      // interrupt time at the frame query
    ULONGLONG        PaceBaseFrame;   // the frame number at that moment
    ULONG            FramesPerMs;     // calibrated counter units (1 = ms frames)
    // Feedback reader: two asynchronous iso IN URBs, resubmitted from their
    // own completion (DPC) with StartFrame = cursor + 32. Without an actively
    // serviced feedback endpoint the asynchronous OUT sink never locks its
    // DAC. The values themselves (the device's frame clock) are M5's input.
    EMU_ISOCH_FB_SLOT Fb[EMU_ISOCH_FB_SLOTS];
    PUCHAR           Buffer;          // nonpaged, EMU_ISOCH_SLOTS * SlotBytes
    EMU_ISOCH_SLOT   Slots[EMU_ISOCH_SLOTS];
    // WaveRT render callbacks. The pump fills from the host ring inside every
    // build (prime included, only after the data gate passed) and reports
    // successful completions.
    PVOID                       CallbackContext;
    PEMU_ISOCH_FILL_RENDER      FillRender;
    PEMU_ISOCH_RENDER_AVAILABLE RenderAvailable;
    PEMU_ISOCH_RENDER_COMPLETED RenderCompleted;
    ULONGLONG                   BaseFrames;
    volatile LONGLONG           SubmittedFrames;
    volatile LONGLONG           CompletedFrames;
    // Counters, all interlocked: the pump and the completion routines write,
    // the stats IOCTL reads, and neither holds a lock against the other.
    volatile LONG    Submits;
    volatile LONG    Completes;
    volatile LONG    UrbFailures;
    volatile LONG    IsoPacketErrors;
    volatile LONG    ShortPackets;    // IN below nominal; OUT below requested
    volatile LONG    Bytes;
    volatile LONG    LastUrbStatus;
    volatile LONG    LastPacketStatus;
};

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
// (StateLock): the start selects alternates and primes the pump, the stop
// retires the stream, aborts the pipes and drains Pending. The completion
// routines touch only stream-owned resources, and the stop path waits for the
// Pending drain before any storage is released, so nothing is freed while an
// IRP of this engine can still be running.

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
// callback receives monotonically increasing positions beginning there and
// the availability callback gates every build on released data.
NTSTATUS
EmuIsochStartRender(
    _Inout_ PEMU_DEVICE_CONTEXT       Ctx,
    _In_ ULONG                        RateHz,
    _In_ ULONGLONG                    BaseFrames,
    _Inout_ PVOID                     CallbackContext,
    _In_ PEMU_ISOCH_FILL_RENDER       FillRender,
    _In_ PEMU_ISOCH_RENDER_AVAILABLE  RenderAvailable,
    _In_ PEMU_ISOCH_RENDER_COMPLETED  RenderCompleted
    );

// Stop render and wait for the Pending drain. The completed count remains
// readable after the stop so RUN -> PAUSE can preserve it in the WaveRT base
// position.
VOID
EmuIsochStopRender(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    );

ULONGLONG
EmuIsochRenderCompletedFrames(
    _In_ PEMU_DEVICE_CONTEXT Ctx
    );

ULONGLONG
EmuIsochRenderSubmittedFrames(
    _In_ PEMU_DEVICE_CONTEXT Ctx
    );

// Stop both directions (idempotent; safe on a never-started instance).
VOID
EmuIsochStopAll(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    );

// The data-arrival pump kick: the WaveRT write-packet path calls it after the
// OS has released a packet into the ring — the analog of the vendor's pump
// call from its KS ring write. Any IRQL; a no-op on an inactive stream.
VOID
EmuIsochRenderDataReady(
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
