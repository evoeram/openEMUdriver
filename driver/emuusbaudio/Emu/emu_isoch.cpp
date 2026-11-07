// EmuUSBAudio — isochronous stream engine, M3 stage B (vendor pump model).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// One direction = one pump, after the disassembly of emusba10.sys
// (FUN_000148A8, docs/ISOCH_PUMP.md). The pump waits for nothing: it is a
// bounded loop that builds and submits URBs while a free slot exists and —
// for render — the data behind the URB is already released by the OS. It is
// called from the vendor's three event sites: data arrival (the WaveRT
// SetWritePacket path), URB completion (DPC — the re-arm happens right there,
// zero gap, no thread), and start/resume. One global StartFrame cursor per
// stream advances +1 per URB and is re-anchored to the bus on every build
// (cursor <= now -> now + 3, the vendor's invariant at 0x4DBE), so a stall of
// any length re-anchors the schedule to the future instead of leaving slots
// firing into the past. An URB without data behind it is never submitted: at
// an empty ring the wire stays silent, exactly like the vendor's break at
// 0x4A60, and the next build re-anchors when data arrives.
//
// The stop path is the load-bearing part: retire under the pump lock, abort
// the pipes so every queued URB completes, wait for the Pending counter to
// drain (each reap drains one), and only then free. Nothing is freed while an
// IRP of this engine can still be running — the plan's rundown rule, obeyed
// literally. A slot whose final completion never arrives is deliberately
// retained rather than freed; the alternative is freeing memory an IRP still
// references.
#include "emu_isoch.h"

// --- pure scheduling helpers -------------------------------------------------
// (nonpaged in the image: the pump calls them per build and the start path
// pages in whatever it needs around them)

BOOLEAN
EmuIsochPeriodMs(
    _In_ ULONG Interval,
    _Out_ PULONG PeriodMs
    )
{
    if (PeriodMs == NULL) {
        return FALSE;
    }
    if (Interval == 0 || Interval > 32) {
        // Zero is not a period, and nothing sane reports more than 32 ms.
        return FALSE;
    }
    if (Interval >= 8 && (Interval % 8) == 0) {
        // High-speed reading: the value counts 125 us units, 8 per frame.
        *PeriodMs = Interval / 8;
        return TRUE;
    }
    // Full-speed reading: whole frames. A 0.125 ms high-speed period (raw
    // Interval 1) also lands here; the engine refuses to schedule it rather
    // than guess, and the breadcrumb shows the raw interval either way.
    *PeriodMs = Interval;
    return TRUE;
}

BOOLEAN
EmuIsochNominalFramesExact(
    _In_ ULONG RateHz,
    _In_ ULONG PeriodMs,
    _Out_ PULONG NominalFrames
    )
{
    ULONG packetsPerSecond;

    if (NominalFrames == NULL || RateHz == 0 || PeriodMs == 0 ||
        (1000 % PeriodMs) != 0) {
        return FALSE;
    }
    packetsPerSecond = 1000 / PeriodMs;
    if ((RateHz % packetsPerSecond) != 0) {
        // 44100 on the 1 ms grid would need 44/45 alternation - M5's feedback
        // engine, not this slice.
        return FALSE;
    }
    *NominalFrames = RateHz / packetsPerSecond;
    return TRUE;
}

ULONG
EmuIsochSlotBytes(
    _In_ ULONG NominalFrames,
    _In_ ULONG BytesPerFrame,
    _In_ ULONG MaxPacketSize
    )
{
    ULONG want = (NominalFrames + EMU_ISOCH_SLOT_HEADROOM) * BytesPerFrame;

    return want > MaxPacketSize ? want : MaxPacketSize;
}

// --- engine ------------------------------------------------------------------

#ifdef EMU_KERNEL
#include "emu.h"
#include <ntstrsafe.h>     // RtlStringCbPrintfW in the pipe-fact breadcrumbs

static VOID
EmuIsochPump(
    _Inout_ PEMU_ISOCH_STREAM S,
    _In_ PEMU_DEVICE_CONTEXT  Ctx
    );

static VOID
EmuIsochFeedbackRearm(
    _Inout_ PEMU_ISOCH_STREAM  S,
    _In_ PEMU_DEVICE_CONTEXT   Ctx,
    _Inout_ PEMU_ISOCH_FB_SLOT Fb
    );

static VOID
EmuIsochReleasePending(
    _Inout_ PEMU_ISOCH_STREAM S
    );

// The interrupt-time mapping of the bus frame: calibrated at start and
// refreshed by every successful query, it is the anchor's source at
// DISPATCH_LEVEL and its fallback at PASSIVE.
static ULONG
EmuIsochMappedFrame(
    _In_ PEMU_ISOCH_STREAM S
    )
{
    ULONGLONG base = S->PaceBaseFrame;
    ULONGLONG now = KeQueryInterruptTime();

    if (now < S->PaceOrigin) {
        return (ULONG)base;
    }
    return (ULONG)(base + (now - S->PaceOrigin) / 10000 * S->FramesPerMs);
}

// The completion of the frame-query IRP: release the query for the next use
// and record the answer. Runs at DISPATCH_LEVEL from the USB stack's DPC —
// possibly inline inside IoCallDriver, which is why it takes no locks.
static NTSTATUS NTAPI
EmuIsochFrameCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp,
    _In_opt_ PVOID      Context
    )
{
    PEMU_ISOCH_STREAM S = (PEMU_ISOCH_STREAM)Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    // The query is accounted in Pending for its whole flight (incremented by
    // the submitter before IoCallDriver), so the stop drain cannot free the
    // IRP while it is still in flight after a floated query.
    InterlockedExchange(&S->FrameBusy, 0);
    EmuIsochReleasePending(S);
    KeSetEvent(&S->FrameEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// The real frame query: submit the dedicated URB and wait. PASSIVE_LEVEL
// ONLY — the wait blocks. Called from the pump's PASSIVE entry (refreshing
// the mapping the DPC anchors live by) and from the start path.
static BOOLEAN
EmuIsochQueryFrameReal(
    _Inout_ PEMU_ISOCH_STREAM S,
    _In_ PEMU_DEVICE_CONTEXT  Ctx,
    _Out_ PULONG              Frame
    )
{
    PIRP              irp = S->FrameIrp;
    PIO_STACK_LOCATION stack;
    LARGE_INTEGER     wait;
    NTSTATUS          status;

    if (irp == NULL ||
        InterlockedCompareExchange(&S->FrameBusy, 1, 0) != 0) {
        InterlockedIncrement(&S->FqBusy);
        return FALSE;
    }

    IoReuseIrp(irp, STATUS_SUCCESS);
    KeClearEvent(&S->FrameEvent);
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    // AssignUrbToIoStackLocation writes both the URB pointer and the
    // IOCTL_INTERNAL_USB_SUBMIT_URB control code into the location. The
    // classic (non-Ex) registration: the routine takes no locks, which is
    // legal at any IRQL, and the Ex variant's PASSIVE APC would bugcheck on
    // an inline completion (the 2.0.0.61 crash class).
    USBD_AssignUrbToIoStackLocation(Ctx->UsbdHandle, stack, S->FrameUrb);
    IoSetCompletionRoutine(irp, EmuIsochFrameCompletion, S, TRUE, TRUE, TRUE);

    // Accounted like any armed URB: the completion drains it. A query that
    // goes pending past the wait below therefore keeps Pending raised, and
    // the stop drain cannot free the IRP out from under the stack.
    InterlockedIncrement(&S->Pending);
    status = IoCallDriver(Ctx->Pdo, irp);
    if (status == STATUS_PENDING) {
        wait.QuadPart = -100 * 10000LL;   // 100 ms bound
        if (KeWaitForSingleObject(&S->FrameEvent, Executive, KernelMode,
                                  FALSE, &wait) != STATUS_SUCCESS) {
            // Still in flight; the completion releases FrameBusy when it
            // runs. The mapping keeps carrying the anchor meanwhile.
            InterlockedIncrement(&S->FqFloat);
            return FALSE;
        }
    }
    if (!NT_SUCCESS(status)) {
        // Completed inline with a failure; the completion has released the
        // query. The mapping stays calibrated from earlier queries.
        InterlockedIncrement(&S->FqFail);
        return FALSE;
    }

    S->FqInline++;
    S->PaceOrigin = KeQueryInterruptTime();
    S->PaceBaseFrame = S->FrameUrb->UrbGetCurrentFrameNumber.FrameNumber;
    *Frame = (ULONG)S->PaceBaseFrame;
    return TRUE;
}

// The current bus frame for the anchor, DISPATCH-safe: the interrupt-time
// mapping only. The vendor re-queries the bus inside every build
// (GET_CURRENT_FRAME_NUMBER in the builder) — legal on the USB2 stack it
// was built for, where the query answers inline without queuing. On the
// USB3 stack the same submit from inside our own completion DPC wedges the
// stack (the pump freeze: submits and completes stop together at the second
// batch of eight, the stream alive, zero failures). So the DPC anchors live
// by the mapping — calibrated at start, refreshed by the real query at every
// PASSIVE pump entry — whose error is clock drift (ppm) over the query
// interval, absorbed by the margin.
static ULONG
EmuIsochBusFrame(
    _In_ PEMU_ISOCH_STREAM S
    )
{
    ULONG now = EmuIsochMappedFrame(S);

    S->NowLast = now;
    return now;
}

// The PASSIVE pump entry: refresh the mapping with a real query. No locks
// held; the pump calls this before its loop.
static VOID
EmuIsochFrameRefresh(
    _Inout_ PEMU_ISOCH_STREAM S,
    _In_ PEMU_DEVICE_CONTEXT  Ctx
    )
{
    ULONG frame;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    (VOID)EmuIsochQueryFrameReal(S, Ctx, &frame);
}

// Read one completed data URB into the counters. DPC-safe: interlocked
// counters, the notification callback's spinlocks, no waits. A render frame
// becomes visible to WaveRT only after its complete packet succeeded on the
// wire; canceled, failed and short packets remain visible in the error
// counters instead. The lengths are the actual transferred bytes — a short
// fill went out as a short packet (the vendor's 0x4BBD) and shows up here.
static VOID
EmuIsochHarvest(
    _Inout_ PEMU_ISOCH_STREAM S,
    _Inout_ PEMU_ISOCH_SLOT   Slot
    )
{
    struct _URB_ISOCH_TRANSFER* u = &Slot->Urb->UrbIsochronousTransfer;
    BOOLEAN out = (S->Direction == EMU_STREAM_RENDER);
    ULONG packetBytes = S->NominalFrames * S->BytesPerFrame;
    LONGLONG completed;
    ULONG got = 0;
    ULONG p;
    BOOLEAN irpSuccess = NT_SUCCESS(Slot->Irp->IoStatus.Status);
    BOOLEAN urbSuccess = NT_SUCCESS((NTSTATUS)u->Hdr.Status);
    BOOLEAN allPacketsOk = TRUE;

    InterlockedIncrement(&S->Completes);

    if (!irpSuccess || !urbSuccess) {
        InterlockedIncrement(&S->UrbFailures);
        InterlockedExchange(&S->LastUrbStatus,
                            !irpSuccess ? (LONG)Slot->Irp->IoStatus.Status
                                        : (LONG)u->Hdr.Status);
    }
    for (p = 0; p < EMU_ISOCH_PKTS_PER_URB; p++) {
        if (!USBD_SUCCESS(u->IsoPacket[p].Status)) {
            InterlockedIncrement(&S->IsoPacketErrors);
            InterlockedExchange(&S->LastPacketStatus,
                                (LONG)u->IsoPacket[p].Status);
            allPacketsOk = FALSE;
        }
        got += u->IsoPacket[p].Length;
    }
    if (got < packetBytes) {
        InterlockedIncrement(&S->ShortPackets);
    }
    InterlockedAdd(&S->Bytes, (LONG)got);

    if (out && irpSuccess && urbSuccess && allPacketsOk &&
        got == packetBytes) {
        // Several slots can complete on different CPUs at once, so the
        // position advances interlocked; the callback gets the new total.
        completed = InterlockedAdd64(&S->CompletedFrames,
                                     (LONGLONG)S->NominalFrames);
        if (S->RenderCompleted != NULL) {
            S->RenderCompleted(S->CallbackContext, (ULONGLONG)completed);
        }
    }
}

// The Pending drain: every reap steps the in-flight counter, and the step to
// zero releases the stop path's wait.
static VOID
EmuIsochReleasePending(
    _Inout_ PEMU_ISOCH_STREAM S
    )
{
    if (InterlockedDecrement(&S->Pending) == 0) {
        KeSetEvent(&S->Idle, IO_NO_INCREMENT, FALSE);
    }
}

// Reap one completed data slot: free it for the pump and drain Pending.
// Idempotent by the ARMED -> FREE exchange: exactly one of the competing
// reapers (the completion routine, the pump after a synchronous completion)
// owns the harvest.
static BOOLEAN
EmuIsochReapSlot(
    _Inout_ PEMU_ISOCH_STREAM S,
    _Inout_ PEMU_ISOCH_SLOT   Slot
    )
{
    if (InterlockedExchange(&Slot->State, EMU_ISOCH_SLOT_FREE) !=
        EMU_ISOCH_SLOT_ARMED) {
        return FALSE;
    }
    EmuIsochHarvest(S, Slot);
    EmuIsochReleasePending(S);
    return TRUE;
}

// The data-URB completion: reap and pump immediately — the vendor's zero-gap
// re-arm happens right here at DISPATCH_LEVEL, no thread, no event, no wake
// latency between completion and the next submission (docs/ISOCH_PUMP.md §4).
static NTSTATUS NTAPI
EmuIsochDataCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp,
    _In_opt_ PVOID      Context
    )
{
    PEMU_ISOCH_SLOT slot = (PEMU_ISOCH_SLOT)Context;
    PEMU_ISOCH_STREAM S = slot->Stream;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    if (EmuIsochReapSlot(S, slot)) {
        EmuIsochPump(S, (PEMU_DEVICE_CONTEXT)S->Owner);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// Reap one completed feedback URB. The values themselves (the device's frame
// clock) are M5's input; today they only feed the counters.
static BOOLEAN
EmuIsochFbReap(
    _Inout_ PEMU_ISOCH_STREAM  S,
    _Inout_ PEMU_ISOCH_FB_SLOT Fb
    )
{
    struct _URB_ISOCH_TRANSFER* u = &Fb->Urb->UrbIsochronousTransfer;

    if (InterlockedExchange(&Fb->State, EMU_ISOCH_SLOT_FREE) !=
        EMU_ISOCH_SLOT_ARMED) {
        return FALSE;
    }
    InterlockedIncrement(&S->Completes);
    if (!NT_SUCCESS((NTSTATUS)u->Hdr.Status) ||
        !USBD_SUCCESS(u->IsoPacket[0].Status)) {
        InterlockedIncrement(&S->IsoPacketErrors);
        InterlockedExchange(&S->LastPacketStatus,
                            (LONG)u->IsoPacket[0].Status);
    }
    InterlockedAdd(&S->Bytes, (LONG)u->IsoPacket[0].Length);
    EmuIsochReleasePending(S);
    return TRUE;
}

// The feedback-URB completion: reap and resubmit — the vendor's self-restart
// (FUN_0001466C). The re-arm chain is depth-bounded: a stack that completed
// every feedback URB inline would otherwise recurse without end.
static NTSTATUS NTAPI
EmuIsochFbCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp,
    _In_opt_ PVOID      Context
    )
{
    PEMU_ISOCH_FB_SLOT fb = (PEMU_ISOCH_FB_SLOT)Context;
    PEMU_ISOCH_STREAM S = fb->Stream;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    if (EmuIsochFbReap(S, fb)) {
        if (InterlockedIncrement(&S->FbRearmDepth) <= 4) {
            EmuIsochFeedbackRearm(S, (PEMU_DEVICE_CONTEXT)S->Owner, fb);
        }
        InterlockedDecrement(&S->FbRearmDepth);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// Per-submit URB fields for a data slot: everything but StartFrame, which the
// pump sets after the anchor. Render data is packed from the host ring right
// here, in the build, under the pump lock — the vendor copies ring->URB at
// build time too (0x4B6B) — and frozen after the submit (§2 of the report).
// The lengths are actual: the OS releases whole packets, so a full URB is the
// rule; anything less goes out short rather than padded (vendor 0x4BBD).
static BOOLEAN
EmuIsochBuildSlot(
    _Inout_ PEMU_ISOCH_STREAM S,
    _Inout_ PEMU_ISOCH_SLOT   Slot,
    _In_    BOOLEAN           Out
    )
{
    struct _URB_ISOCH_TRANSFER* u = &Slot->Urb->UrbIsochronousTransfer;
    PUCHAR transferBuffer = S->Buffer +
        (ULONG)(Slot - &S->Slots[0]) * S->SlotBytes;
    ULONG packetBytes = S->NominalFrames * S->BytesPerFrame;
    ULONG perPacket = S->NominalFrames / EMU_ISOCH_PKTS_PER_URB;
    ULONG p;

    u->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    // The URB must cover every packet descriptor: the base struct ends with
    // IsoPacket[1], so a two-packet URB is one descriptor longer. 2.0.0.69
    // declared NumberOfPackets=2 in a one-packet-sized URB and the stack
    // silently dropped the second packet (every transfer "short").
    u->Hdr.Length = (USHORT)(sizeof(struct _URB_ISOCH_TRANSFER) +
        (EMU_ISOCH_PKTS_PER_URB - 1) * sizeof(USBD_ISO_PACKET_DESCRIPTOR));
    u->Hdr.Status = 0;
    u->PipeHandle = S->Pipe;
    u->TransferFlags = Out
        ? USBD_TRANSFER_DIRECTION_OUT
        : (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK);
    u->TransferBuffer = transferBuffer;
    u->TransferBufferMDL = NULL;
    u->NumberOfPackets = EMU_ISOCH_PKTS_PER_URB;

    if (Out && S->FillRender != NULL) {
        // The byte offsets are cumulative over the ACTUAL previous lengths
        // (vendor 0x4B24..0x4B3E), and TransferBufferLength is the sum — the
        // underrun shape of the trace (144 / 138), not a fixed 288.
        ULONGLONG pos = S->BaseFrames + (ULONGLONG)S->SubmittedFrames;
        ULONG off = 0;
        ULONG frames = 0;

        for (p = 0; p < EMU_ISOCH_PKTS_PER_URB; p++) {
            ULONG want = (p == EMU_ISOCH_PKTS_PER_URB - 1)
                       ? S->NominalFrames - frames : perPacket;
            ULONG got = S->FillRender(S->CallbackContext,
                                      transferBuffer + off, want,
                                      pos + frames);

            u->IsoPacket[p].Offset = off;
            u->IsoPacket[p].Length = got * S->BytesPerFrame;
            u->IsoPacket[p].Status = 0;
            off += got * S->BytesPerFrame;
            frames += got;
        }
        if (frames == 0) {
            // Nothing went in: the wire stays silent (the vendor pump would
            // not have reached the builder at all — gate 0x4A60).
            return FALSE;
        }
        u->TransferBufferLength = frames * S->BytesPerFrame;
        // Only the pump writes this counter. It describes data consumed into
        // USB slot buffers, never the externally visible playback position.
        S->SubmittedFrames += frames;
        return TRUE;
    }

    // Capture and the probe path (no fill callback): fixed nominal layout.
    // An IN slot must be able to hold whatever the device sends; the probe
    // buffer is pre-filled once and repeats every pipeline cycle.
    u->TransferBufferLength = Out ? packetBytes : S->SlotBytes;
    for (p = 0; p < EMU_ISOCH_PKTS_PER_URB; p++) {
        u->IsoPacket[p].Offset = p * (packetBytes / EMU_ISOCH_PKTS_PER_URB);
        u->IsoPacket[p].Length = (Out ? packetBytes : S->SlotBytes) /
                                 EMU_ISOCH_PKTS_PER_URB;
        u->IsoPacket[p].Status = 0;
    }
    if (Out) {
        S->SubmittedFrames += S->NominalFrames;
    }
    return TRUE;
}

// The pump: the vendor FUN_000148A8. Builds and submits URBs while a free
// slot exists and — for render with a data source — the ring holds at least
// one URB of released data. Any IRQL: called from PASSIVE write paths and
// from the completion DPC. Serialized by PumpLock. The submit itself happens
// OUTSIDE the lock: a stack that completes inline would run the completion
// routine — which re-enters the pump — inside IoCallDriver, and a spinlock is
// not recursive.
static VOID
EmuIsochPump(
    _Inout_ PEMU_ISOCH_STREAM S,
    _In_ PEMU_DEVICE_CONTEXT  Ctx
    )
{
    BOOLEAN out = (S->Direction == EMU_STREAM_RENDER);
    ULONG   syncSpins = 0;
    KIRQL   oldIrql;

    // Single flight. An inline completion re-enters the pump on the same
    // kernel stack (IoCallDriver -> completion -> pump); without this guard
    // a stack that completes every submit synchronously — the xHCI behavior
    // for an URB whose StartFrame is already in the past, or any
    // deterministic validation rejection — recurses without bound and
    // overflows the DPC stack (the 0x7F/0x8 double fault of the first pump
    // build: slots free up again on every inline reap, so the pool never
    // bounds the chain). A nested call returns immediately; the enclosing
    // loop below keeps draining, so no completion is lost.
    if (InterlockedCompareExchange(&S->Pumping, 1, 0) != 0) {
        return;
    }

    // The mapping refresh: a real bus query, PASSIVE only (no locks held
    // here). The DPC anchors live by the refreshed mapping afterwards.
    EmuIsochFrameRefresh(S, Ctx);

    for (;;) {
        PEMU_ISOCH_SLOT slot = NULL;
        struct _URB_ISOCH_TRANSFER* u;
        PIO_STACK_LOCATION stack;
        ULONG now;
        ULONG i;
        NTSTATUS status;

        KeAcquireSpinLock(&S->PumpLock, &oldIrql);
        if (!S->Active || S->Quit != 0) {
            KeReleaseSpinLock(&S->PumpLock, oldIrql);
            InterlockedIncrement(&S->PumpQuitExit);
            break;
        }
        for (i = 0; i < EMU_ISOCH_SLOTS; i++) {
            if (S->Slots[i].State == EMU_ISOCH_SLOT_FREE) {
                slot = &S->Slots[i];
                break;
            }
        }
        if (slot == NULL) {
            // Pool exhausted: the pipeline is full (vendor break at 0x4A9E).
            KeReleaseSpinLock(&S->PumpLock, oldIrql);
            InterlockedIncrement(&S->PumpPoolExit);
            break;
        }

        // Data gate (vendor break at 0x4A60): an URB without data behind it
        // is never submitted. Below one URB of released frames the pump
        // leaves and the wire stays silent until the next write packet; the
        // anchor in the next build resumes the schedule in the future.
        if (out && S->FillRender != NULL && S->RenderAvailable != NULL) {
            ULONGLONG pos = S->BaseFrames + (ULONGLONG)S->SubmittedFrames;

            if (S->RenderAvailable(S->CallbackContext, pos) <
                (ULONGLONG)S->NominalFrames) {
                if (InterlockedCompareExchange(&S->Starved, 1, 0) == 0) {
                    InterlockedIncrement(&S->Underruns);
                }
                KeReleaseSpinLock(&S->PumpLock, oldIrql);
                InterlockedIncrement(&S->PumpGateExit);
                break;
            }
        }
        InterlockedExchange(&S->Starved, 0);

        if (!EmuIsochBuildSlot(S, slot, out)) {
            KeReleaseSpinLock(&S->PumpLock, oldIrql);
            InterlockedIncrement(&S->PumpBuildExit);
            break;
        }

        // The anchor, on EVERY build (vendor 0x4DBE): a cursor that reached
        // the present jumps to the future. This is the invariant that makes
        // any stall self-healing — a URB scheduled behind the bus frame is
        // completed by the controller immediately, the observed xHCI
        // behavior, and would never recover on its own. The `now` here is
        // the interrupt-time mapping (see EmuIsochBusFrame): no bus submits
        // from inside the completion DPC.
        now = EmuIsochBusFrame(S);
        if ((LONG)(S->CursorFrame - now) <= 0) {
            S->CursorFrame = now + EMU_ISOCH_FRAME_MARGIN;
            S->AnchorFires++;
        }
        u = &slot->Urb->UrbIsochronousTransfer;
        u->StartFrame = S->CursorFrame;
        S->CursorFrame += 1;   // the vendor's +1 frame per URB (trace-verified)
        S->CursorLast = S->CursorFrame;

        IoReuseIrp(slot->Irp, STATUS_SUCCESS);
        stack = IoGetNextIrpStackLocation(slot->Irp);
        stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        USBD_AssignUrbToIoStackLocation(Ctx->UsbdHandle, stack, slot->Urb);
        IoSetCompletionRoutine(slot->Irp, EmuIsochDataCompletion, slot,
                               TRUE, TRUE, TRUE);

        slot->State = EMU_ISOCH_SLOT_ARMED;
        InterlockedIncrement(&S->Pending);
        InterlockedIncrement(&S->Submits);
        KeReleaseSpinLock(&S->PumpLock, oldIrql);

        status = IoCallDriver(Ctx->Pdo, slot->Irp);
        if (status != STATUS_PENDING) {
            // The stack completed inline: the completion routine has already
            // run inside IoCallDriver on this same thread — reaped the slot,
            // harvested the real statuses, and re-entered the pump (refused
            // by the single-flight guard above; this loop continues the
            // drain). Touching the slot here would steal the ARMED state of
            // a URB a nested pump may have armed, so the slot lifecycle is
            // entirely the completion's business. A stack that returns
            // without completing is contract-broken; the slot wedges ARMED,
            // the stop drain trips and FreeStream retains the stream.
            if (!NT_SUCCESS(status) &&
                InterlockedCompareExchange(&S->LastUrbStatus, 0, 0) !=
                    (LONG)status) {
                InterlockedIncrement(&S->UrbFailures);
                InterlockedExchange(&S->LastUrbStatus, (LONG)status);
            }
            // Spin bound: a run of inline completions means either a
            // deterministic rejection (retrying inside this call is futile,
            // the counters show it) or instant completions of past-scheduled
            // URBs (pure CPU burn at DISPATCH). Cycle the pool twice at most,
            // then leave the rest to the next pump event.
            if (++syncSpins >= EMU_ISOCH_SYNC_SPIN_LIMIT) {
                break;
            }
        } else {
            syncSpins = 0;
        }
        // Loop: re-acquire at the top and keep draining the ring.
    }
    InterlockedExchange(&S->Pumping, 0);
}

// Arm one feedback URB: two slots keep the endpoint serviced, consecutive
// StartFrames exactly EMU_ISOCH_FB_STEP (32) apart — the trace's feedback
// delta — and always in the future. The cursor re-anchors whenever it fell
// into the past (the vendor's sync builder, FUN_00013E98 @0x3F1B). The
// submit happens outside the pump lock, like the data pump.
static VOID
EmuIsochFeedbackRearm(
    _Inout_ PEMU_ISOCH_STREAM  S,
    _In_ PEMU_DEVICE_CONTEXT   Ctx,
    _Inout_ PEMU_ISOCH_FB_SLOT Fb
    )
{
    struct _URB_ISOCH_TRANSFER* u;
    PIO_STACK_LOCATION stack;
    KIRQL oldIrql;
    ULONG now;
    NTSTATUS status;

    KeAcquireSpinLock(&S->PumpLock, &oldIrql);
    if (S->FeedbackPipe == NULL || !S->Active || S->Quit != 0) {
        KeReleaseSpinLock(&S->PumpLock, oldIrql);
        return;
    }

    now = EmuIsochBusFrame(S);
    if ((LONG)(S->FbCursorFrame - now) <= 0) {
        S->FbCursorFrame = now + EMU_ISOCH_FRAME_MARGIN;
    }
    u = &Fb->Urb->UrbIsochronousTransfer;
    u->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    u->Hdr.Length = (USHORT)sizeof(struct _URB_ISOCH_TRANSFER);
    u->Hdr.Status = 0;
    u->PipeHandle = S->FeedbackPipe;
    u->TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    u->TransferBuffer = Fb->Buffer;
    u->TransferBufferLength = 4;
    S->FbCursorFrame += EMU_ISOCH_FB_STEP;
    u->StartFrame = S->FbCursorFrame;
    u->NumberOfPackets = 1;
    u->IsoPacket[0].Offset = 0;
    u->IsoPacket[0].Length = 4;
    u->IsoPacket[0].Status = 0;

    IoReuseIrp(Fb->Irp, STATUS_SUCCESS);
    stack = IoGetNextIrpStackLocation(Fb->Irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    USBD_AssignUrbToIoStackLocation(Ctx->UsbdHandle, stack, Fb->Urb);
    IoSetCompletionRoutine(Fb->Irp, EmuIsochFbCompletion, Fb, TRUE, TRUE, TRUE);

    Fb->State = EMU_ISOCH_SLOT_ARMED;
    InterlockedIncrement(&S->Pending);
    InterlockedIncrement(&S->Submits);
    KeReleaseSpinLock(&S->PumpLock, oldIrql);

    status = IoCallDriver(Ctx->Pdo, Fb->Irp);
    if (status != STATUS_PENDING) {
        // Completed inline: the completion reaped and (depth permitting)
        // re-armed already, on this same thread. The slot lifecycle stays
        // the completion's — see the data pump for the contract analysis.
        if (!NT_SUCCESS(status) &&
            InterlockedCompareExchange(&S->LastUrbStatus, 0, 0) !=
                (LONG)status) {
            InterlockedIncrement(&S->UrbFailures);
            InterlockedExchange(&S->LastUrbStatus, (LONG)status);
        }
    }
}

// The OUT pattern: a 24-bit counter ramp, both channels carrying the same
// value. Deterministic, recognizable on a wiretap, no floating point. This is
// probe instrumentation for the no-callback start paths; the M4 WaveRT path
// fills from client data instead.
static VOID
EmuIsochFillOutPattern(
    _Inout_ PEMU_ISOCH_STREAM S
    )
{
    PUCHAR p = S->Buffer;
    ULONG  total = EMU_ISOCH_SLOTS * S->NominalFrames;
    ULONG  channels = S->BytesPerFrame / 3;
    ULONG  value = 0;

    RtlZeroMemory(S->Buffer, (SIZE_T)EMU_ISOCH_SLOTS * S->SlotBytes);
    for (ULONG f = 0; f < total; f++) {
        UCHAR b0 = (UCHAR)(value & 0xFF);
        UCHAR b1 = (UCHAR)((value >> 8) & 0xFF);
        UCHAR b2 = (UCHAR)((value >> 16) & 0xFF);
        for (ULONG c = 0; c < channels; c++) {
            *p++ = b0;
            *p++ = b1;
            *p++ = b2;
        }
        value++;
    }
}

// The data-arrival kick for the WaveRT write-packet path: the analog of the
// vendor's pump call from its KS ring write (FUN_00013424). Any IRQL; a
// no-op on an inactive stream.
VOID
EmuIsochRenderDataReady(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    )
{
    PEMU_ISOCH_STREAM S = &Ctx->Isoch[EMU_STREAM_RENDER];

    if (S->Active && S->Quit == 0) {
        EmuIsochPump(S, Ctx);
    }
}

// Free what a stream owns. Every normal call site has already retired the
// stream and drained Pending. A stuck completion retains the entire stream;
// StartStream rejects reuse until the device instance is gone rather than
// overwriting a slot or freeing memory that the USB stack can still access.
#pragma code_seg("PAGE")
static VOID
EmuIsochFreeStream(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ ULONG                  Direction
    )
{
    PEMU_ISOCH_STREAM S = &Ctx->Isoch[Direction];
    ULONG i;

    PAGED_CODE();

    if (S->Pending != 0) {
        EMU_LOG_ERROR(STR_MODULENAME "isoch dir %lu retained: completion still "
                      "in flight at free\n", Direction);
        return;
    }

    for (i = 0; i < EMU_ISOCH_SLOTS; i++) {
        PEMU_ISOCH_SLOT slot = &S->Slots[i];

        if (slot->Irp != NULL) {
            IoFreeIrp(slot->Irp);
            slot->Irp = NULL;
        }
        if (slot->Urb != NULL) {
            USBD_UrbFree(Ctx->UsbdHandle, slot->Urb);
            slot->Urb = NULL;
        }
    }
    for (i = 0; i < EMU_ISOCH_FB_SLOTS; i++) {
        PEMU_ISOCH_FB_SLOT fb = &S->Fb[i];

        if (fb->Irp != NULL) {
            IoFreeIrp(fb->Irp);
            fb->Irp = NULL;
        }
        if (fb->Urb != NULL) {
            USBD_UrbFree(Ctx->UsbdHandle, fb->Urb);
            fb->Urb = NULL;
        }
        if (fb->Buffer != NULL) {
            ExFreePoolWithTag(fb->Buffer, EMU_POOLTAG);
            fb->Buffer = NULL;
        }
    }
    if (S->FrameIrp != NULL) {
        IoFreeIrp(S->FrameIrp);
        S->FrameIrp = NULL;
    }
    if (S->FrameUrb != NULL) {
        USBD_UrbFree(Ctx->UsbdHandle, S->FrameUrb);
        S->FrameUrb = NULL;
    }
    if (S->Buffer != NULL) {
        ExFreePoolWithTag(S->Buffer, EMU_POOLTAG);
        S->Buffer = NULL;
    }
    S->Active = FALSE;
    S->Quit = 0;
    S->Pending = 0;
    S->Starved = 0;
    S->Underruns = 0;
    S->Pumping = 0;
    S->FbRearmDepth = 0;
    S->FqInline = 0;
    S->FqFloat = 0;
    S->FqFail = 0;
    S->FqBusy = 0;
    S->AnchorFires = 0;
    S->NowLast = 0;
    S->CursorLast = 0;
    S->PumpQuitExit = 0;
    S->PumpPoolExit = 0;
    S->PumpGateExit = 0;
    S->PumpBuildExit = 0;
    S->FrameBusy = 0;
    S->CursorFrame = 0;
    S->FbCursorFrame = 0;
    S->Pipe = NULL;
    S->FeedbackPipe = NULL;
    S->CallbackContext = NULL;
    S->FillRender = NULL;
    S->RenderAvailable = NULL;
    S->RenderCompleted = NULL;
    S->BaseFrames = 0;
    S->SubmittedFrames = 0;
    // CompletedFrames survives until the next start: WaveRT reads it after the
    // drain to preserve RUN -> PAUSE -> RUN positioning.
}

// Retire and stop one direction: quit flag under the pump lock, pipe aborts,
// Pending drain, then free. The abort is a bus operation and belongs to a
// live device; on the release path (device possibly gone) it fails and the
// drain still runs - a canceled or dead URB completes either way.
static VOID
EmuIsochStopStream(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ ULONG                  Direction
    )
{
    PEMU_ISOCH_STREAM         S = &Ctx->Isoch[Direction];
    PURB                      abortUrb = NULL;
    LARGE_INTEGER             wait;
    KIRQL                     oldIrql;
    ULONG                     waits = 0;
    NTSTATUS                  status;

    PAGED_CODE();

    if (!S->Active) {
        return;
    }

    // Retire first, under the pump lock: every pump site (write path at
    // PASSIVE, completion at DISPATCH) rechecks under the lock and stops
    // arming. Anything already armed is accounted in Pending.
    KeAcquireSpinLock(&S->PumpLock, &oldIrql);
    S->Quit = 1;
    S->Active = FALSE;
    KeReleaseSpinLock(&S->PumpLock, oldIrql);

    // The wire verdict of the pump: gate-blocked passes since start, the
    // frame-query outcomes, the anchor's view, and why the pump stopped.
    {
        EmuNoteStatusValue(Ctx, Direction == EMU_STREAM_RENDER
                                ? L"PumpUnderruns" : L"PumpUnderrunsC",
                           (ULONG)S->Underruns);
        EmuNoteStatusValue(Ctx, L"FqOk", (ULONG)S->FqInline);
        EmuNoteStatusValue(Ctx, L"FqFloat", (ULONG)S->FqFloat);
        EmuNoteStatusValue(Ctx, L"FqFail", (ULONG)S->FqFail);
        EmuNoteStatusValue(Ctx, L"FqBusy", (ULONG)S->FqBusy);
        EmuNoteStatusValue(Ctx, L"AnchorFires", (ULONG)S->AnchorFires);
        EmuNoteStatusValue(Ctx, L"NowLast", S->NowLast);
        EmuNoteStatusValue(Ctx, L"CursorLast", S->CursorLast);
        EmuNoteStatusValue(Ctx, L"PumpQuitExit", (ULONG)S->PumpQuitExit);
        EmuNoteStatusValue(Ctx, L"PumpPoolExit", (ULONG)S->PumpPoolExit);
        EmuNoteStatusValue(Ctx, L"PumpGateExit", (ULONG)S->PumpGateExit);
        EmuNoteStatusValue(Ctx, L"PumpBuildExit", (ULONG)S->PumpBuildExit);
    }

    if (S->Pipe != NULL) {
        // Contract-602 tracks URB allocations in the USBD handle. A stack URB
        // is rejected by USB3 with bugcheck 0x144/3 before the request runs.
        status = USBD_UrbAllocate(Ctx->UsbdHandle, &abortUrb);
        if (NT_SUCCESS(status)) {
            // Abort every queued transfer on the data pipe: the drain below
            // depends on each of them completing, canceled or not.
            abortUrb->UrbPipeRequest.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
            abortUrb->UrbPipeRequest.Hdr.Length =
                (USHORT)sizeof(struct _URB_PIPE_REQUEST);
            abortUrb->UrbPipeRequest.Hdr.Status = 0;
            abortUrb->UrbPipeRequest.PipeHandle = S->Pipe;
            status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, abortUrb);
            // EmuUsbSubmitUrb is synchronous, so the allocation is no longer
            // referenced by its IRP at this point.
            USBD_UrbFree(Ctx->UsbdHandle, abortUrb);
        }
        if (!NT_SUCCESS(status)) {
            EMU_LOG_WARN(STR_MODULENAME "abort pipe (dir %lu) failed, 0x%x\n",
                         Direction, status);
        }
    }
    if (S->FeedbackPipe != NULL) {
        status = USBD_UrbAllocate(Ctx->UsbdHandle, &abortUrb);
        if (NT_SUCCESS(status)) {
            abortUrb->UrbPipeRequest.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
            abortUrb->UrbPipeRequest.Hdr.Length =
                (USHORT)sizeof(struct _URB_PIPE_REQUEST);
            abortUrb->UrbPipeRequest.Hdr.Status = 0;
            abortUrb->UrbPipeRequest.PipeHandle = S->FeedbackPipe;
            (VOID)EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, abortUrb);
            USBD_UrbFree(Ctx->UsbdHandle, abortUrb);
        }
    }

    // Drain: after the aborts every armed URB completes; each reap drains
    // Pending and the last one sets Idle. The timeout is a tripwire only - a
    // stream that outlives it is retained, never freed.
    wait.QuadPart = -1 * 1000 * 10000LL;   // 1 s per wait, 3 s total
    while (InterlockedCompareExchange(&S->Pending, 0, 0) != 0) {
        status = KeWaitForSingleObject(&S->Idle, Executive, KernelMode, FALSE,
                                       &wait);
        if (++waits >= 3) {
            EMU_LOG_ERROR(STR_MODULENAME "isoch dir %lu did not drain, "
                          "pending %ld\n", Direction,
                          InterlockedCompareExchange(&S->Pending, 0, 0));
            break;
        }
    }
    EmuIsochFreeStream(Ctx, Direction);
}

VOID
EmuIsochStopAll(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    )
{
    PAGED_CODE();

    EmuIsochStopStream(Ctx, EMU_STREAM_RENDER);
    EmuIsochStopStream(Ctx, EMU_STREAM_CAPTURE);
}

VOID
EmuIsochStopRender(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    )
{
    PAGED_CODE();

    EmuIsochStopStream(Ctx, EMU_STREAM_RENDER);
}

ULONGLONG
EmuIsochRenderCompletedFrames(
    _In_ PEMU_DEVICE_CONTEXT Ctx
    )
{
    return (ULONGLONG)InterlockedCompareExchange64(
        &Ctx->Isoch[EMU_STREAM_RENDER].CompletedFrames, 0, 0);
}

ULONGLONG
EmuIsochRenderSubmittedFrames(
    _In_ PEMU_DEVICE_CONTEXT Ctx
    )
{
    return (ULONGLONG)InterlockedCompareExchange64(
        &Ctx->Isoch[EMU_STREAM_RENDER].SubmittedFrames, 0, 0);
}

// Arm one direction: select the alternate, read the pipe facts out of the
// submitted select-interface URB, build the slots, prime the pump.
static NTSTATUS
EmuIsochStartStream(
    _Inout_ PEMU_DEVICE_CONTEXT   Ctx,
    _In_ ULONG                    Direction,
    _In_ const EMU_STREAM_ALT*    Alt,
    _In_ ULONG                    RateHz
    )
{
    PEMU_ISOCH_STREAM            S = &Ctx->Isoch[Direction];
    PUSBD_INTERFACE_INFORMATION  iface;
    PUSBD_PIPE_INFORMATION       dataPipe = NULL;
    PUSBD_PIPE_INFORMATION       feedbackPipe = NULL;
    ULONG                        base;
    BOOLEAN                      out = (Direction == EMU_STREAM_RENDER);
    NTSTATUS                     status;
    KIRQL                        oldIrql;
    ULONG                        i;

    PAGED_CODE();

    if (S->Active || S->Pending != 0) {
        // A retained stuck stream cannot be reused without overwriting storage
        // that an outstanding USB request may still reference.
        return STATUS_DEVICE_BUSY;
    }

    status = EmuUsbSelectInterface(Ctx, Direction,
                                   Alt->InterfaceNumber, Alt->AltSetting);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    iface = Ctx->Stream[Direction].Interface;
    if (iface == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    // Pipe-visibility breadcrumb: full per-pipe facts as separate values.
    // PipeInfoK: (endpoint << 24) | (type << 16) | (mps << 0), and PipeUsK:
    // the raw Interval. The feedback endpoint (usage=feedback, 0x11 in
    // bmAttributes, wMaxPacketSize 4) must be among them or the reader has
    // nothing to read. The 2.0.0.60 nibble packing was ambiguous; the first
    // pipe's nibble read as 9 - an endpoint the descriptor does not have.
    {
        ULONG npipes = iface->NumberOfPipes;

        if (npipes > 8) {
            npipes = 8;
        }
        for (ULONG p = 0; p < npipes; p++) {
            PUSBD_PIPE_INFORMATION pipe = &iface->Pipes[p];
            WCHAR name[32];

            RtlStringCbPrintfW(name, sizeof(name),
                               Direction == EMU_STREAM_RENDER
                                   ? L"PipeInfoR%lu" : L"PipeInfoC%lu", p);
            EmuNoteStatusValue(Ctx, name,
                               ((ULONG)pipe->EndpointAddress << 24) |
                               ((ULONG)pipe->PipeType << 16) |
                               (pipe->MaximumPacketSize & 0xFFFF));
            RtlStringCbPrintfW(name, sizeof(name),
                               Direction == EMU_STREAM_RENDER
                                   ? L"PipeUsR%lu" : L"PipeUsC%lu", p);
            EmuNoteStatusValue(Ctx, name, pipe->Interval);
        }
    }

    // The data pipe is the one that matches the direction; on the render
    // interface the IN isochronous pipe next to the OUT is the feedback
    // endpoint, serviced by the feedback slots rather than streamed.
    for (ULONG p = 0; p < iface->NumberOfPipes; p++) {
        PUSBD_PIPE_INFORMATION pipe = &iface->Pipes[p];
        BOOLEAN isIn = (pipe->EndpointAddress & USB_ENDPOINT_DIRECTION_MASK) != 0;

        if (pipe->PipeType != UsbdPipeTypeIsochronous) {
            continue;
        }
        // Direction decides, no skipping: on the render interface the IN
        // isochronous pipe next to the OUT is the feedback endpoint, and the
        // old `isIn == out` skip rejected every IN pipe before the feedback
        // branch could ever take it - TestFb0 was 0 since M3-B (2.0.0.60
        // breadcrumb PipesR proved the pipe was there all along).
        if (isIn == (Direction == EMU_STREAM_CAPTURE)) {
            if (dataPipe == NULL) {
                dataPipe = pipe;
            }
        } else if (feedbackPipe == NULL) {
            feedbackPipe = pipe;
        }
    }
    if (dataPipe == NULL) {
        EMU_LOG_ERROR(STR_MODULENAME "dir %lu: no isochronous data pipe in "
                      "alt %lu\n", Direction, Alt->AltSetting);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (Alt->SubFrameSize != 3 || Alt->BitResolution != 24) {
        // The M3 test slice streams packed 24-bit only; other formats are the
        // M5/M6 world's business.
        EMU_LOG_ERROR(STR_MODULENAME "dir %lu: alt %lu is %lu bit in %lu "
                      "bytes, test slice streams 24/3 only\n",
                      Direction, Alt->AltSetting, Alt->BitResolution,
                      Alt->SubFrameSize);
        return STATUS_INVALID_PARAMETER;
    }

    S->Owner = Ctx;
    S->Direction = Direction;
    S->RateHz = RateHz;
    S->SubmittedFrames = 0;
    InterlockedExchange64(&S->CompletedFrames, 0);
    InterlockedExchange(&S->Submits, 0);
    InterlockedExchange(&S->Completes, 0);
    InterlockedExchange(&S->UrbFailures, 0);
    InterlockedExchange(&S->IsoPacketErrors, 0);
    InterlockedExchange(&S->ShortPackets, 0);
    InterlockedExchange(&S->Bytes, 0);
    InterlockedExchange(&S->LastUrbStatus, 0);
    InterlockedExchange(&S->LastPacketStatus, 0);
    S->Pipe = dataPipe->PipeHandle;
    S->FeedbackPipe = feedbackPipe != NULL ? feedbackPipe->PipeHandle : NULL;
    S->MaxPacketSize = dataPipe->MaximumPacketSize;
    S->PipeInterval = dataPipe->Interval;
    if (!EmuIsochPeriodMs(S->PipeInterval, &S->PeriodMs)) {
        EMU_LOG_ERROR(STR_MODULENAME "dir %lu: pipe interval %lu is not a "
                      "period this engine can schedule\n",
                      Direction, S->PipeInterval);
        EmuIsochFreeStream(Ctx, Direction);
        return STATUS_INVALID_PARAMETER;
    }
    // The wire grid, byte-for-byte the vendor's (trace + disassembly of
    // emusba10): each URB covers ONE millisecond frame and carries TWO
    // 24-frame packets (microframes 0 and 4 of the bInterval=3 endpoint; the
    // USBD PipeInformation.Interval reads 4 = microframes per packet), the
    // schedule leads the current frame by 3, eight URBs in flight, one frame
    // step per URB. The feedback endpoint reports 24 frames per 500-us window
    // (16.16 value 0x00180000) and is serviced by two async URBs.
    S->PeriodUs = 1000;
    S->FramesPerPacket = RateHz / 1000;   // 48: frames per URB (both packets)
    if (S->FramesPerPacket < EMU_ISOCH_PKTS_PER_URB) {
        S->FramesPerPacket = EMU_ISOCH_PKTS_PER_URB;
    }
    S->NominalFrames = S->FramesPerPacket;
    S->PeriodMs = 1;   // legacy breadcrumb field
    S->BytesPerFrame = Alt->Channels * Alt->SubFrameSize;
    S->SlotBytes = EmuIsochSlotBytes(S->NominalFrames, S->BytesPerFrame,
                                     S->MaxPacketSize);

    // The boot-session verdict reads these: raw interval, derived period and
    // the pipe's maximum, packed into one DWORD per direction.
    EmuNoteStatusValue(Ctx, Direction == EMU_STREAM_RENDER
                            ? L"TestPipe0" : L"TestPipe1",
                       (S->PeriodMs << 24) | (S->PipeInterval << 16) |
                       (S->MaxPacketSize & 0xFFFF));
    EmuNoteStatusValue(Ctx, Direction == EMU_STREAM_RENDER
                            ? L"TestFb0" : L"TestFb1",
                       feedbackPipe != NULL ? 1 : 0);

    S->Buffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                        (SIZE_T)EMU_ISOCH_SLOTS * S->SlotBytes,
                                        EMU_POOLTAG);
    if (S->Buffer == NULL) {
        EmuIsochFreeStream(Ctx, Direction);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (out && S->FillRender == NULL) {
        // Probe instrumentation: the wire is about to get the deterministic
        // ramp instead of client data - inaudible near-DC, perfect counters,
        // total silence. This value in the registry identifies that state.
        EmuNoteStatusValue(Ctx, L"PatternFill", 1);
        EmuIsochFillOutPattern(S);
    } else {
        // WaveRT slots are filled afresh by the pump; capture and any
        // not-yet-submitted headroom begin as silence rather than stale data.
        RtlZeroMemory(S->Buffer, (SIZE_T)EMU_ISOCH_SLOTS * S->SlotBytes);
    }

    for (i = 0; i < EMU_ISOCH_SLOTS; i++) {
        PEMU_ISOCH_SLOT slot = &S->Slots[i];

        RtlZeroMemory(slot, sizeof(*slot));
        slot->Stream = S;
        slot->State = EMU_ISOCH_SLOT_FREE;
        status = USBD_IsochUrbAllocate(Ctx->UsbdHandle,
                                       EMU_ISOCH_PKTS_PER_URB, &slot->Urb);
        if (!NT_SUCCESS(status)) {
            EmuIsochFreeStream(Ctx, Direction);
            return status;
        }
        slot->Irp = IoAllocateIrp(Ctx->Pdo->StackSize, FALSE);
        if (slot->Irp == NULL) {
            EmuIsochFreeStream(Ctx, Direction);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    // The feedback reader's two slots: the asynchronous OUT sink never locks
    // its DAC unless this endpoint is being serviced (the 2.0.0.23-2.0.0.58
    // silence had every other wire parameter matching the vendor trace byte
    // for byte). The vendor keeps two iso IN URBs in flight, resubmitted from
    // their own completion.
    for (i = 0; i < EMU_ISOCH_FB_SLOTS && S->FeedbackPipe != NULL; i++) {
        PEMU_ISOCH_FB_SLOT fb = &S->Fb[i];

        RtlZeroMemory(fb, sizeof(*fb));
        fb->Stream = S;
        fb->State = EMU_ISOCH_SLOT_FREE;
        fb->Buffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, 8,
                                             EMU_POOLTAG);
        if (fb->Buffer == NULL) {
            EmuIsochFreeStream(Ctx, Direction);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(fb->Buffer, 8);
        status = USBD_IsochUrbAllocate(Ctx->UsbdHandle, 1, &fb->Urb);
        if (!NT_SUCCESS(status)) {
            EmuIsochFreeStream(Ctx, Direction);
            return status;
        }
        fb->Irp = IoAllocateIrp(Ctx->Pdo->StackSize, FALSE);
        if (fb->Irp == NULL) {
            EmuIsochFreeStream(Ctx, Direction);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    // The frame-number query pair: a persistent URB (the shape is set once)
    // and a dedicated IRP the pump reuses per anchor. This URB must come from
    // the contract-602 handle too: the 2.0.0.24 stack-local frameUrb caused
    // bugcheck 0x144/3 on the first hardware START.
    status = USBD_UrbAllocate(Ctx->UsbdHandle, &S->FrameUrb);
    if (!NT_SUCCESS(status)) {
        EmuIsochFreeStream(Ctx, Direction);
        return status;
    }
    S->FrameUrb->UrbGetCurrentFrameNumber.Hdr.Function =
        URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
    S->FrameUrb->UrbGetCurrentFrameNumber.Hdr.Length =
        (USHORT)sizeof(struct _URB_GET_CURRENT_FRAME_NUMBER);
    S->FrameUrb->UrbGetCurrentFrameNumber.Hdr.Status = 0;
    KeInitializeEvent(&S->FrameEvent, NotificationEvent, FALSE);
    S->FrameBusy = 0;
    S->FrameIrp = IoAllocateIrp(Ctx->Pdo->StackSize, FALSE);
    if (S->FrameIrp == NULL) {
        EmuIsochFreeStream(Ctx, Direction);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeInitializeSpinLock(&S->PumpLock);
    KeInitializeEvent(&S->Idle, NotificationEvent, FALSE);
    S->Pending = 0;
    S->Quit = 0;
    S->Starved = 0;
    S->Underruns = 0;
    S->Pumping = 0;
    S->FbRearmDepth = 0;
    S->FqInline = 0;
    S->FqFloat = 0;
    S->FqFail = 0;
    S->FqBusy = 0;
    S->AnchorFires = 0;
    S->NowLast = 0;
    S->CursorLast = 0;
    S->PumpQuitExit = 0;
    S->PumpPoolExit = 0;
    S->PumpGateExit = 0;
    S->PumpBuildExit = 0;

    // Calibrate the counter units: query twice across 10 ms of interrupt
    // time. Millisecond frames give delta 10; a microframe-based counter
    // gives 80. The anchor mapping scales with it - wrong units schedule
    // every packet in the past, the controller completes them all at once,
    // and the wire runs many times faster than real audio.
    status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, S->FrameUrb);
    if (NT_SUCCESS(status)) {
        ULONG f1 = S->FrameUrb->UrbGetCurrentFrameNumber.FrameNumber;
        LARGE_INTEGER calDelay;

        calDelay.QuadPart = -10 * 10000LL;   // 10 ms
        KeDelayExecutionThread(KernelMode, FALSE, &calDelay);
        S->FrameUrb->UrbGetCurrentFrameNumber.Hdr.Status = 0;
        status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, S->FrameUrb);
        if (NT_SUCCESS(status)) {
            ULONG f2 = S->FrameUrb->UrbGetCurrentFrameNumber.FrameNumber;
            ULONG delta = f2 > f1 ? f2 - f1 : 1;

            S->FramesPerMs = delta / 10;
            if (S->FramesPerMs == 0) {
                S->FramesPerMs = 1;
            }
            EmuNoteStatusValue(Ctx, Direction == EMU_STREAM_RENDER
                                    ? L"CalFramesPerMs" : L"CalFramesPerMsC",
                               S->FramesPerMs);
        }
    }
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "current frame query failed, 0x%x\n",
                      status);
        EmuIsochFreeStream(Ctx, Direction);
        return status;
    }
    // Reserve the frame window: current frame plus the margin (the vendor's
    // FUN_00015278 start write: cursor = GET_CURRENT_FRAME_NUMBER + 3). The
    // per-build reverification keeps it there; late submissions are counted,
    // not hidden.
    base = S->FrameUrb->UrbGetCurrentFrameNumber.FrameNumber +
           EMU_ISOCH_FRAME_MARGIN;
    S->PaceOrigin = KeQueryInterruptTime();
    S->PaceBaseFrame = S->FrameUrb->UrbGetCurrentFrameNumber.FrameNumber;
    S->CursorFrame = base;
    S->FbCursorFrame = base;
    // The wire verdict: the first StartFrame, for correlating a bus trace.
    EmuNoteStatusValue(Ctx, Direction == EMU_STREAM_RENDER
                            ? L"TestFrame0" : L"TestFrame1", base);

    // SYNC_RESET_PIPE_AND_CLEAR_STALL on both pipes before the first URB:
    // the vendor start sequence does exactly this (FUN_00019B20), dropping
    // whatever controller state the previous driver session left behind.
    {
        PURB resetUrb = NULL;

        if (NT_SUCCESS(USBD_UrbAllocate(Ctx->UsbdHandle, &resetUrb))) {
            resetUrb->UrbPipeRequest.Hdr.Function =
                URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL;
            resetUrb->UrbPipeRequest.Hdr.Length =
                (USHORT)sizeof(struct _URB_PIPE_REQUEST);
            resetUrb->UrbPipeRequest.Hdr.Status = 0;
            resetUrb->UrbPipeRequest.PipeHandle = S->Pipe;
            (VOID)EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, resetUrb);
            if (S->FeedbackPipe != NULL) {
                resetUrb->UrbPipeRequest.PipeHandle = S->FeedbackPipe;
                (VOID)EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, resetUrb);
            }
            USBD_UrbFree(Ctx->UsbdHandle, resetUrb);
        }
    }

    KeAcquireSpinLock(&S->PumpLock, &oldIrql);
    S->Active = TRUE;
    KeReleaseSpinLock(&S->PumpLock, oldIrql);

    // Prime through the pump: with an empty ring the data gate blocks and
    // NOT ONE URB goes out (the vendor's OUT start: RESET_PIPE, then wait
    // for data — no prime with zeros). Capture and the probe pattern have no
    // gate and arm the full pipeline immediately.
    EmuIsochPump(S, Ctx);
    for (i = 0; i < EMU_ISOCH_FB_SLOTS; i++) {
        EmuIsochFeedbackRearm(S, Ctx, &S->Fb[i]);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
EmuIsochStartRender(
    _Inout_ PEMU_DEVICE_CONTEXT       Ctx,
    _In_ ULONG                        RateHz,
    _In_ ULONGLONG                    BaseFrames,
    _Inout_ PVOID                     CallbackContext,
    _In_ PEMU_ISOCH_FILL_RENDER       FillRender,
    _In_ PEMU_ISOCH_RENDER_AVAILABLE  RenderAvailable,
    _In_ PEMU_ISOCH_RENDER_COMPLETED  RenderCompleted
    )
{
    const EMU_STREAM_ALT* alt;
    PEMU_ISOCH_STREAM S = &Ctx->Isoch[EMU_STREAM_RENDER];
    NTSTATUS status;

    PAGED_CODE();

    if (RateHz != 48000 || CallbackContext == NULL || FillRender == NULL ||
        RenderAvailable == NULL || RenderCompleted == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (S->Active) {
        return STATUS_DEVICE_BUSY;
    }
    alt = EmuCapsFindAlt(Ctx->Caps, EMU_STREAM_RENDER, 2, RateHz);
    if (alt == NULL) {
        return STATUS_NO_MATCH;
    }

    // The device D/A clocks from the XU clock-rate register: the stock driver
    // and v1 SET it before streaming, and a device left at another rate gates
    // its output (silent DAC). The StartDevice probe only reads. Set the
    // streaming rate on every start. The wave start path holds StateLock
    // across EmuIsochStartRender, so this must be the Locked variant: the
    // plain EmuUsbExecuteSet would re-acquire the caller's fast mutex and
    // wait on itself forever - the 2.0.0.24 bug class, reintroduced here once
    // already and caught before the 2.0.0.40 hardware run. A failed SET fails
    // the start loudly: continuing with an unknown device rate can produce
    // silently distorted audio instead.
    EmuNoteStatusValue(Ctx, L"RateSetEnter", 1);
    status = EmuUsbExecuteSetLocked(Ctx, EmuIoctlSetRate, RateHz);
    EmuNoteStatusValue(Ctx, L"StreamRateSet", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        EmuNoteStatusValue(Ctx, L"WaveStartStatus", (ULONG)status);
        return status;
    }

    // Unconditional mute-off on the master feature unit. The device keeps
    // mute in its nonvolatile state and has no USB volume (the front-panel
    // knob is the only gain): a device that last met the stock panel muted
    // streams perfectly and stays silent - the no-sound report of the first
    // 2.0.0.40 hardware run. Not fatal when it fails: the stream is
    // independent of it, and the breadcrumb records the wire verdict.
    if (Ctx->Caps->FeatureUnitCount > 0) {
        EMU_CTRL_REQUEST muteRequest;

        status = EmuCtrlBuildMuteOff(&muteRequest,
                                     Ctx->Caps->FeatureUnits[0].UnitID,
                                     Ctx->Caps->FeatureUnits[0].InterfaceNumber);
        if (NT_SUCCESS(status)) {
            status = EmuUsbSendControl(Ctx, &muteRequest, muteRequest.Payload);
        }
        EmuNoteStatusValue(Ctx, L"MuteSetStatus", (ULONG)status);

        // Per-channel volume = 0 dB, ALL volume-capable channels. The device
        // keeps these in its nonvolatile memory; a unit last seen by the
        // stock panel at -inf is silent through any stream despite the
        // unmute above. The 0404 feature unit covers four output channels
        // (main L/R + headphone L/R) and the two-channel stream may map to
        // any pair of them - set every one. Master (channel 0) has no volume
        // control and is skipped. Status bytes: low byte = channel 1, then
        // 2, 3, 4. Not fatal: the stream is independent of it.
        if (Ctx->Caps->FeatureUnits[0].ChannelCount >= 1) {
            EMU_CTRL_REQUEST volRequest;
            ULONG volStatus = 0;
            ULONG channels = Ctx->Caps->FeatureUnits[0].ChannelCount;

            if (channels > 4) {
                channels = 4;
            }
            for (LONG ch = (LONG)channels; ch >= 1; ch--) {
                status = EmuCtrlBuildVolumeSet(&volRequest,
                                               Ctx->Caps->FeatureUnits[0].UnitID,
                                               Ctx->Caps->FeatureUnits[0].InterfaceNumber,
                                               (ULONG)ch, 0);
                if (NT_SUCCESS(status)) {
                    status = EmuUsbSendControl(Ctx, &volRequest,
                                               volRequest.Payload);
                }
                volStatus = (volStatus << 8) | (UCHAR)(status & 0xFF);
            }
            EmuNoteStatusValue(Ctx, L"VolSetStatus", volStatus);
        }
    }

    S->CallbackContext = CallbackContext;
    S->FillRender = FillRender;
    S->RenderAvailable = RenderAvailable;
    // Probe instrumentation: the assignment happened - the absence of this
    // value with a successful start means the run never reached this line.
    EmuNoteStatusValue(Ctx, L"FillSet", 0x10000001);
    S->RenderCompleted = RenderCompleted;
    S->BaseFrames = BaseFrames;
    status = EmuIsochStartStream(Ctx, EMU_STREAM_RENDER, alt, RateHz);
    if (!NT_SUCCESS(status)) {
        S->CallbackContext = NULL;
        S->FillRender = NULL;
        S->RenderAvailable = NULL;
        S->RenderCompleted = NULL;
        S->BaseFrames = 0;
    }
    EmuNoteStatusValue(Ctx, L"WaveStartStatus", (ULONG)status);
    return status;
}

NTSTATUS
EmuIsochStartStreams(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ ULONG                  RateHz
    )
{
    const EMU_STREAM_ALT* alt[2];
    NTSTATUS              status;

    PAGED_CODE();

    // The M3 test contract: 48 kHz stereo, nothing else. Wider support is the
    // feedback engine's (fractional packets) and the WaveRT path's (client
    // formats) business.
    if (RateHz != 48000) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Ctx->Isoch[0].Active || Ctx->Isoch[1].Active) {
        // Restart semantics: a running test stream is replaced, not doubled.
        EmuIsochStopAll(Ctx);
    }

    // Both alternates must exist before anything changes on the device.
    alt[0] = EmuCapsFindAlt(Ctx->Caps, EMU_STREAM_RENDER, 2, RateHz);
    alt[1] = EmuCapsFindAlt(Ctx->Caps, EMU_STREAM_CAPTURE, 2, RateHz);
    if (alt[0] == NULL || alt[1] == NULL) {
        EMU_LOG_ERROR(STR_MODULENAME "no 2ch %lu Hz alt pair\n", RateHz);
        return STATUS_INVALID_PARAMETER;
    }

    // The rate goes through the existing SET path: descriptor veto, EP0
    // SET_CUR, cache note - the exact operation the panel contract drives.
    // The dispatch holds StateLock across the whole start; the Locked variant
    // is required here, the plain one would re-acquire the caller's mutex and
    // wait on itself forever (the first hardware run of this IOCTL hung
    // exactly there, before any of the breadcrumbs below could be written).
    status = EmuUsbExecuteSetLocked(Ctx, EmuIoctlSetRate, RateHz);
    // The wire verdict of the SET step, separate from TestStartStatus: if a
    // later start hangs again, the absence of this value pins the hang to the
    // EP0 request rather than to the selects or the stream engine.
    EmuNoteStatusValue(Ctx, L"TestSetStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = EmuIsochStartStream(Ctx, EMU_STREAM_RENDER, alt[0], RateHz);
    if (!NT_SUCCESS(status)) {
        EmuIsochStopAll(Ctx);
        return status;
    }
    status = EmuIsochStartStream(Ctx, EMU_STREAM_CAPTURE, alt[1], RateHz);
    if (!NT_SUCCESS(status)) {
        EmuIsochStopAll(Ctx);
        return status;
    }
    EmuNoteStatusValue(Ctx, L"TestStartStatus", (ULONG)STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

VOID
EmuIsochFillStats(
    _Out_ PEMU_TEST_STREAM_STATS Out,
    _In_ PEMU_DEVICE_CONTEXT     Ctx
    )
{
    PAGED_CODE();

    Out->Size = sizeof(EMU_TEST_STREAM_STATS);
    Out->Version = EMU_IOCTL_VERSION;
    // The rate of whichever direction is running; both are armed together, so
    // the first says it for the pair, and zero means nothing is armed.
    Out->RateHz = Ctx->Isoch[0].Active ? Ctx->Isoch[0].RateHz
                : Ctx->Isoch[1].Active ? Ctx->Isoch[1].RateHz : 0;

    for (ULONG d = 0; d < 2; d++) {
        PEMU_ISOCH_STREAM         S = &Ctx->Isoch[d];
        PEMU_TEST_STREAM_DIR_STATS e = &Out->Dir[d];

        e->Running = S->Active ? 1 : 0;
        if (S->Active) {
            e->InterfaceNumber = Ctx->Stream[d].InterfaceNumber;
            e->AltSetting = Ctx->Stream[d].AltSetting;
            e->MaxPacketSize = S->MaxPacketSize;
            e->PipeInterval = S->PipeInterval;
            e->PeriodMs = S->PeriodMs;
            e->NominalFrames = S->NominalFrames;
        } else {
            // Idle reports "no value" rather than the stale facts of the last
            // session - the same honesty the settings cache is built on.
            e->InterfaceNumber = EMU_NO_VALUE;
            e->AltSetting = EMU_NO_VALUE;
            e->MaxPacketSize = 0;
            e->PipeInterval = 0;
            e->PeriodMs = 0;
            e->NominalFrames = 0;
        }
        e->Submits = (ULONG)InterlockedCompareExchange(&S->Submits, 0, 0);
        e->Completes = (ULONG)InterlockedCompareExchange(&S->Completes, 0, 0);
        e->UrbFailures = (ULONG)InterlockedCompareExchange(&S->UrbFailures, 0, 0);
        e->IsoPacketErrors =
            (ULONG)InterlockedCompareExchange(&S->IsoPacketErrors, 0, 0);
        e->ShortPackets =
            (ULONG)InterlockedCompareExchange(&S->ShortPackets, 0, 0);
        e->Bytes = (ULONG)InterlockedCompareExchange(&S->Bytes, 0, 0);
        e->LastUrbStatus =
            (ULONG)InterlockedCompareExchange(&S->LastUrbStatus, 0, 0);
        e->LastPacketStatus =
            (ULONG)InterlockedCompareExchange(&S->LastPacketStatus, 0, 0);
    }
}
#pragma code_seg()

#endif // EMU_KERNEL
