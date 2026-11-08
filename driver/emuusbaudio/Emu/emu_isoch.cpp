// EmuUSBAudio — isochronous stream engine, M3 stage B (vendor pump model).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// One direction = one pump, after the disassembly of emusba10.sys
// (FUN_000148A8, docs/ISOCH_PUMP.md). The pump waits for nothing: it is a
// bounded loop that builds and submits URBs while a free slot exists and —
// for render — the data behind the URB is already released by the OS. It is
// called from the vendor's three event sites: data arrival (the WaveRT
// SetWritePacket path), URB completion (DPC — the data re-arm happens right
// there, zero gap, no thread), and start/resume. One global StartFrame cursor
// per stream advances +1 per URB and is re-anchored to the bus on every build
// (cursor <= now -> now + 3, the vendor's invariant at 0x4DBE), so a stall of
// any length re-anchors the schedule to the future instead of leaving slots
// firing into the past. The anchor also clamps a cursor that leads the bus by
// more than EMU_ISOCH_MAX_LEAD back to the present: the vendor's invariant
// only corrects a cursor that fell BEHIND, and the 0x133 dump showed the
// feedback loop marching its cursor ~66 h ahead of the bus while every
// re-arm cycled inside one DPC. The feedback re-arm itself never resubmits
// from inside its completion — the completion sets a 1 ms timer whose DPC
// does the submit, so one DPC carries at most one feedback submission and
// every cycle counter (the old FbRearmDepth) is gone. An URB without data
// behind it is never submitted: at an empty ring the wire stays silent,
// exactly like the vendor's break at 0x4A60, and the next build re-anchors
// when data arrives.
//
// The engine is bounded fail-stop: the first URB-level contract break (submit
// rejection, feedback failure, failed data URB) is recorded one-shot into
// FailStage/FailStatus and sets Quit — the pump and the feedback slots stop
// arming, StopStream performs the normal retire/drain/free. Per-packet
// "missed service" statuses stay counted-only: they are the normal underrun
// class, and the wire-side answer to them is the honest position advance in
// EmuIsochHarvest (the device consumed the frame slots even when the packets
// were silence), not a shutdown.
//
// The stop path is the load-bearing part: retire under the pump lock, abort
// the pipes, cancel the owned IRPs the aborts cannot reach, wait for the
// Pending counter to drain (each reap drains one), and only then free.
// Nothing is freed while an IRP of this engine can still be running — the
// plan's rundown rule, obeyed literally. A slot whose final completion never
// arrives is deliberately retained rather than freed; the alternative is
// freeing memory an IRP still references.
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

LONG
EmuIsochCursorAnchor(
    _In_ ULONG  Now,
    _Inout_ volatile ULONG *Cursor
    )
{
    LONG lead = (LONG)(*Cursor - Now);

    if (lead <= 0) {
        // The vendor's invariant at 0x4DBE: a cursor that reached the present
        // jumps to the future — a stall of any length is self-healing.
        *Cursor = Now + EMU_ISOCH_FRAME_MARGIN;
        return 1;
    }
    if (lead > (LONG)EMU_ISOCH_MAX_LEAD) {
        // The half of the invariant the vendor never needed: a cursor that
        // outran the bus (the feedback re-arm storm of the 0x133 dump) is
        // clamped back to the present instead of scheduling further and
        // further ahead.
        *Cursor = Now + EMU_ISOCH_FRAME_MARGIN;
        return 2;
    }
    return 0;
}

BOOLEAN
EmuIsochFramesPerMsValid(
    _In_ ULONG FramesPerMs
    )
{
    return FramesPerMs >= 1 && FramesPerMs <= 16;
}

ULONG
EmuIsochPickAltIndex(
    _In_reads_(Count) const EMU_STREAM_ALT* Alts,
    _In_ ULONG Count,
    _In_ ULONG Direction,
    _In_ ULONG Channels,
    _In_ ULONG RateHz
    )
{
    ULONG first = Count;
    ULONG wanted = Count;
    ULONG i;

    for (i = 0; i < Count; i++) {
        const EMU_STREAM_ALT* alt = &Alts[i];
        BOOLEAN rate = FALSE;

        if (alt->Direction != Direction || alt->Channels != Channels) {
            continue;
        }
        // The rate check mirrors EmuAltSupportsRate (emu_desc.cpp); it is
        // repeated here because the pure section links without emu_desc.cpp.
        if (alt->RateFlags & EMU_ALT_CONTINUOUS_RATES) {
            rate = (BOOLEAN)(RateHz >= alt->ContinuousMinHz &&
                             RateHz <= alt->ContinuousMaxHz);
        } else {
            for (ULONG r = 0; r < alt->RateCount; r++) {
                if (alt->Rates[r] == RateHz) {
                    rate = TRUE;
                    break;
                }
            }
        }
        if (!rate) {
            continue;
        }
        if (first == Count) {
            first = i;   // the descriptor-order first match (the old pick)
        }
        if (alt->DataInterval == EMU_ISOCH_DATA_BINTERVAL) {
            wanted = i;  // the grid match: nothing better exists
            break;
        }
    }
    return wanted != Count ? wanted : first;
}

// The iso packet status lives in the USBD_STATUS namespace: signed LONG,
// negative means error (usb.h's USBD_SUCCESS). Restated here because the pure
// section links without usb.h, and the unit test runs the same rule in
// user mode.
static BOOLEAN
EmuIsochPacketStatusOk(
    _In_ ULONG Status
    )
{
    return (BOOLEAN)(((LONG)Status) >= 0);
}

BOOLEAN
EmuIsochPlanFixedNominal(
    _In_ ULONG NominalFrames,
    _In_ ULONG BytesPerFrame,
    _In_ ULONG MaxPacketSize,
    _In_ BOOLEAN Out,
    _Out_ PULONG TransferBufferLength,
    _Out_writes_(EMU_ISOCH_PKTS_PER_URB) PEMU_ISOCH_PACKET_PLAN Plan
    )
{
    ULONG packetBytes;
    ULONG bufferBytes;
    ULONG p;

    if (TransferBufferLength == NULL || Plan == NULL) {
        return FALSE;
    }
    // Descriptor-shaped bounds: nothing a parsed alt can produce comes near
    // them, and inside them every product below stays far from a ULONG wrap.
    // (EmuIsochSlotBytes itself is unchecked on purpose - the start path
    // feeds it real pipe facts - so the guard lives here, at the planner.)
    if (NominalFrames < EMU_ISOCH_PKTS_PER_URB || NominalFrames > 4096 ||
        BytesPerFrame == 0 || BytesPerFrame > 64 ||
        MaxPacketSize == 0 || MaxPacketSize > 65536) {
        return FALSE;
    }

    packetBytes = NominalFrames * BytesPerFrame;
    bufferBytes = Out ? packetBytes
                      : EmuIsochSlotBytes(NominalFrames, BytesPerFrame,
                                          MaxPacketSize);
    // The wire layout of the current engine, verbatim: the packet stride is
    // the nominal audio of one packet, the per-packet length is half of the
    // whole transfer buffer (payload bytes for OUT, capacity for IN). For a
    // capture slot the two disagree whenever the slot holds headroom above
    // the nominal audio - that is the P1 layout suspicion, kept byte-identical
    // here until the hardware verdict (docs/v3_plan.md P1, ISOCH_PUMP.md).
    *TransferBufferLength = bufferBytes;
    for (p = 0; p < EMU_ISOCH_PKTS_PER_URB; p++) {
        Plan[p].Offset = p * (packetBytes / EMU_ISOCH_PKTS_PER_URB);
        Plan[p].Length = bufferBytes / EMU_ISOCH_PKTS_PER_URB;
    }
    return TRUE;
}

BOOLEAN
EmuIsochValidateLayout(
    _In_ ULONG TransferBufferLength,
    _In_ ULONG MaxPacketSize,
    _In_reads_(PacketCount) const EMU_ISOCH_PACKET_PLAN* Plan,
    _In_ ULONG PacketCount,
    _Out_ PULONG FirstBad
    )
{
    ULONG prevEnd = 0;
    ULONG i;

    if (FirstBad == NULL) {
        return FALSE;
    }
    *FirstBad = 0;
    if (Plan == NULL || PacketCount == 0) {
        return FALSE;
    }

    for (i = 0; i < PacketCount; i++) {
        ULONGLONG end = (ULONGLONG)Plan[i].Offset + (ULONGLONG)Plan[i].Length;

        // The stack schedules what the descriptors say, packet by packet:
        //   - a packet may not claim more than the endpoint's packet size
        //     (the wire cannot carry it - the P1.2 capacity rule);
        //   - the packet must end inside the transfer buffer;
        //   - the packet must start at or after the end of the previous one:
        //     iso descriptors are placed, not scattered, and two packets
        //     claiming the same bytes is the overlap class (the P1 suspect).
        // A zero-length packet is legal anywhere from the previous end
        // onwards: an OUT tail that got no data, an IN capacity of zero.
        if (Plan[i].Length > MaxPacketSize ||
            end > (ULONGLONG)TransferBufferLength ||
            Plan[i].Offset < prevEnd) {
            *FirstBad = i;
            return FALSE;
        }
        prevEnd = (ULONG)end;
    }
    *FirstBad = PacketCount;
    return TRUE;
}

VOID
EmuIsochEvaluateCompletion(
    _In_ ULONG NominalFrames,
    _In_ ULONG BytesPerFrame,
    _In_ NTSTATUS IrpStatus,
    _In_ NTSTATUS UrbStatus,
    _In_reads_(EMU_ISOCH_PKTS_PER_URB) const ULONG* PacketLength,
    _In_reads_(EMU_ISOCH_PKTS_PER_URB) const ULONG* PacketStatus,
    _Out_ PEMU_ISOCH_COMPLETION_SUMMARY Summary
    )
{
    ULONG total = 0;
    ULONG errors = 0;
    ULONG lastBad = 0;
    ULONG p;

    Summary->TotalBytes = 0;
    Summary->ExpectedBytes = 0;
    Summary->Short = FALSE;
    Summary->PacketErrors = 0;
    Summary->LastPacketStatus = 0;
    Summary->UrbFailed = FALSE;
    Summary->FailStatus = (NTSTATUS)0;
    Summary->FramesTransferred = 0;

    if (PacketLength == NULL || PacketStatus == NULL) {
        return;
    }

    // Byte and error accounting, exactly as the harvest reads the completed
    // descriptors: every packet's Length counts (a failed packet's partial
    // bytes are just as gone from the schedule), every failing status is
    // counted and remembered.
    for (p = 0; p < EMU_ISOCH_PKTS_PER_URB; p++) {
        total += PacketLength[p];
        if (!EmuIsochPacketStatusOk(PacketStatus[p])) {
            errors++;
            lastBad = PacketStatus[p];
        }
    }
    Summary->TotalBytes = total;
    Summary->PacketErrors = errors;
    Summary->LastPacketStatus = lastBad;
    Summary->ExpectedBytes = NominalFrames * BytesPerFrame;
    Summary->Short = (BOOLEAN)(total < Summary->ExpectedBytes);
    Summary->UrbFailed = (BOOLEAN)(!NT_SUCCESS(IrpStatus) ||
                                   !NT_SUCCESS(UrbStatus));
    // The IRP verdict wins when both failed: it is the one the stack sets
    // for cancellation and submission errors, and the harvest records it the
    // same way.
    Summary->FailStatus = !NT_SUCCESS(IrpStatus) ? IrpStatus : UrbStatus;
    if (BytesPerFrame != 0) {
        Summary->FramesTransferred = total / BytesPerFrame;
    }
}

// --- engine ------------------------------------------------------------------

#ifdef EMU_KERNEL
#include "emu.h"
#include <usbioctl.h>    // IOCTL_INTERNAL_USB_SUBMIT_URB
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

// Bounded fail-stop: record the FIRST contract break and stop the stream from
// arming anything new. Any IRQL (completions, timer DPC, pump): the verdict
// is one-shot under the pump lock, the same lock the pump and the feedback
// sites recheck their Quit under. Active is deliberately left set: StopStream
// must still run its retire/drain/free over the stream, and clearing Active
// here would make the stop path return early at its !S->Active gate. A break
// discovered after the stop has already retired the stream (a canceled URB
// completing with STATUS_CANCELLED, for instance) is a cancel artifact, not a
// verdict, and records nothing.
static VOID
EmuIsochFailStop(
    _Inout_ PEMU_ISOCH_STREAM S,
    _In_ LONG                 Stage,
    _In_ NTSTATUS             Status
    )
{
    KIRQL oldIrql;

    // Lock-free first check: the common no-op path (a verdict already stands)
    // must not take the spinlock on every completion of a failed stream.
    if (InterlockedCompareExchange(&S->FailStage, 0, 0) != EMU_ISOCH_FAIL_NONE) {
        return;
    }
    KeAcquireSpinLock(&S->PumpLock, &oldIrql);
    if (S->Quit != 0 || S->FailStage != EMU_ISOCH_FAIL_NONE) {
        KeReleaseSpinLock(&S->PumpLock, oldIrql);
        return;
    }
    S->FailStage = Stage;
    S->FailStatus = (LONG)Status;
    S->Quit = 1;
    KeReleaseSpinLock(&S->PumpLock, oldIrql);
}

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
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    // AssignUrbToIoStackLocation associates the URB; the IOCTL code must
    // be set independently for the USB stack. The
    // classic (non-Ex) registration: the routine takes no locks, which is
    // legal at any IRQL, and the Ex variant's PASSIVE APC would bugcheck on
    // an inline completion (the 2.0.0.61 crash class).
    USBD_AssignUrbToIoStackLocation(Ctx->UsbdHandle, stack, S->FrameUrb);
    IoSetCompletionRoutine(irp, EmuIsochFrameCompletion, S, TRUE, TRUE, TRUE);

    // Accounted like any armed URB: the completion drains it. A query that
    // goes pending past the wait below therefore keeps Pending raised, and
    // the stop drain cannot free the IRP out from under the stack. The Idle
    // latch is cleared on the 0 -> 1 step the same way every arm site does.
    if (InterlockedCompareExchange(&S->Pending, 0, 0) == 0) {
        KeClearEvent(&S->Idle);
    }
    InterlockedIncrement(&S->Pending);
    status = IoCallDriver(Ctx->Pdo, irp);
    if (status == STATUS_PENDING) {
        wait.QuadPart = -100 * 10000LL;   // 100 ms bound
        if (KeWaitForSingleObject(&S->FrameEvent, Executive, KernelMode,
                                  FALSE, &wait) != STATUS_SUCCESS) {
            // Still in flight: cancel and give the stack one bounded second
            // to finish the cancellation. The old code left the query in
            // flight here — Pending stayed raised until a completion that
            // might never come, the stop drain timed out and the whole
            // stream was retained (the floated-query wedge).
            InterlockedIncrement(&S->FqFloat);
            IoCancelIrp(irp);
            wait.QuadPart = -1 * 1000 * 10000LL;
            if (KeWaitForSingleObject(&S->FrameEvent, Executive, KernelMode,
                                      FALSE, &wait) != STATUS_SUCCESS) {
                // Even the cancel did not complete. The IRP stays accounted
                // in Pending and FrameBusy stays set until the completion
                // finally runs; the stop path cancels again. Nothing here
                // touches the URB — the stack owns it until completion.
                return FALSE;
            }
        }
    }

    // The query's real verdict. IoCallDriver's status is stale once a floated
    // query completed after the wait — STATUS_PENDING itself passes
    // NT_SUCCESS, and the old code then fell into the success path and read a
    // URB the stack had failed to fill, anchoring the whole mapping to a
    // stale or zero frame number (the past-scheduling runaway class). Both
    // the IRP and the URB status must say success before the answer moves the
    // anchor.
    if (!NT_SUCCESS(irp->IoStatus.Status) ||
        !NT_SUCCESS((NTSTATUS)S->FrameUrb->UrbGetCurrentFrameNumber.Hdr.Status)) {
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

// The P1 diagnostics row: one per completed data URB, planned vs actual per
// packet plus the verdicts. DPC-safe: the completion owns the slot exclusively
// until the reap (the State exchange happens after this), so the URB and the
// stored plan are stable; the row is claimed by an interlocked ticket, so two
// CPUs completing in parallel never write one row; Seq is stored last with a
// full barrier, which makes every earlier field visible to the reader that
// sees it.
static VOID
EmuIsochDiagRecord(
    _Inout_ PEMU_ISOCH_STREAM S,
    _Inout_ PEMU_ISOCH_SLOT   Slot,
    _In_    NTSTATUS          IrpStatus
    )
{
    struct _URB_ISOCH_TRANSFER* u = &Slot->Urb->UrbIsochronousTransfer;
    ULONG n = (ULONG)InterlockedIncrement(&S->DiagSeq);
    PEMU_ISOCH_DIAG_ROW row = &S->Diag[(n - 1) % EMU_ISOCH_DIAG_ROWS];
    ULONG p;

    row->StartFrame = u->StartFrame;
    row->IrpStatus = (ULONG)IrpStatus;
    row->UrbStatus = (ULONG)u->Hdr.Status;
    for (p = 0; p < EMU_ISOCH_PKTS_PER_URB; p++) {
        row->P[p][0] = Slot->Plan[p].Offset;
        row->P[p][1] = Slot->Plan[p].Length;
        row->P[p][2] = u->IsoPacket[p].Length;
        row->P[p][3] = (ULONG)u->IsoPacket[p].Status;
    }
    InterlockedExchange((volatile LONG*)&row->Seq, (LONG)n);
}

// Read one completed data URB into the counters. DPC-safe: interlocked
// counters, the notification callback's spinlocks, no waits. The render
// position advances by the frames the wire actually carried on EVERY
// completion — canceled, failed and short packets stay visible in the error
// counters, but the frame slots they consumed are gone either way, and a
// frozen position would strand the WaveRT notification handshake (the
// reported hang). The lengths are the actual transferred bytes — a short fill
// went out as a short packet (the vendor's 0x4BBD) and shows up here.
static VOID
EmuIsochHarvest(
    _Inout_ PEMU_ISOCH_STREAM S,
    _Inout_ PEMU_ISOCH_SLOT   Slot
    )
{
    struct _URB_ISOCH_TRANSFER* u = &Slot->Urb->UrbIsochronousTransfer;
    BOOLEAN out = (S->Direction == EMU_STREAM_RENDER);
    ULONG lengths[EMU_ISOCH_PKTS_PER_URB];
    ULONG statuses[EMU_ISOCH_PKTS_PER_URB];
    EMU_ISOCH_COMPLETION_SUMMARY sum;
    ULONG p;
    LONGLONG completed;

    InterlockedIncrement(&S->Completes);

    for (p = 0; p < EMU_ISOCH_PKTS_PER_URB; p++) {
        lengths[p] = u->IsoPacket[p].Length;
        statuses[p] = (ULONG)u->IsoPacket[p].Status;
    }
    EmuIsochEvaluateCompletion(S->NominalFrames, S->BytesPerFrame,
                               Slot->Irp->IoStatus.Status,
                               (NTSTATUS)u->Hdr.Status,
                               lengths, statuses, &sum);

    if (sum.UrbFailed) {
        InterlockedIncrement(&S->UrbFailures);
        InterlockedExchange(&S->LastUrbStatus, (LONG)sum.FailStatus);
        // A URB-level failure is a contract break, not a wire condition; a
        // run of them stops the stream (canceled completions at stop record
        // nothing — FailStop checks Quit). One broken URB can be a stack
        // hiccup; EMU_ISOCH_URB_FAIL_LIMIT in a row cannot.
        if (InterlockedIncrement(&S->UrbConsecErrors) >=
            EMU_ISOCH_URB_FAIL_LIMIT) {
            EmuIsochFailStop(S, EMU_ISOCH_FAIL_URB, sum.FailStatus);
        }
    } else {
        InterlockedExchange(&S->UrbConsecErrors, 0);
    }
    if (sum.PacketErrors > 0) {
        InterlockedAdd(&S->IsoPacketErrors, (LONG)sum.PacketErrors);
        InterlockedExchange(&S->LastPacketStatus, (LONG)sum.LastPacketStatus);
    }
    if (sum.Short) {
        InterlockedIncrement(&S->ShortPackets);
    }
    InterlockedAdd(&S->Bytes, (LONG)sum.TotalBytes);

    // The P1 wire history: what was planned, what arrived, with which status.
    // Both directions record it - the same instrument serves the render runs
    // and the capture investigation, and neither pays more than a few stores
    // per completion.
    EmuIsochDiagRecord(S, Slot, Slot->Irp->IoStatus.Status);

    if (out) {
        // The position advance is every completion, error or not: the device
        // consumed the frame slots even when the packets were missed (it
        // played silence), and a completion that froze the position froze the
        // player clock and stranded the notification handshake — the OS waits
        // for the WaveRT events, and those fire from nowhere else. That was
        // the reported hang. The advance is the frames the wire actually
        // carried, so a short URB advances what it sent and CompletedFrames
        // never passes SubmittedFrames.
        //
        // Several slots can complete on different CPUs at once, so the
        // position advances interlocked; the callback gets the new total.
        completed = InterlockedAdd64(&S->CompletedFrames,
                                     (LONGLONG)sum.FramesTransferred);
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

// Reap one completed data slot: harvest first, then free the slot for the
// pump, then drain Pending. The order is the rundown-safe one: the pump only
// rebuilds FREE slots (it scans under PumpLock), so the URB the harvest reads
// can never be rebuilt or resubmitted under it. An IRP completes exactly
// once, so the ARMED exchange below is a tripwire against a double reap, not
// an ownership gate — the harvest has already run when it fires. (The old
// FREE-before-harvest order let the pump rebuild the slot while the statuses
// were still being read; every error counter raced.)
static BOOLEAN
EmuIsochReapSlot(
    _Inout_ PEMU_ISOCH_STREAM S,
    _Inout_ PEMU_ISOCH_SLOT   Slot
    )
{
    EmuIsochHarvest(S, Slot);
    if (InterlockedExchange(&Slot->State, EMU_ISOCH_SLOT_FREE) !=
        EMU_ISOCH_SLOT_ARMED) {
        return FALSE;
    }
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
// clock) are M5's input; today they only feed the counters. Same
// harvest-before-FREE order as the data slots.
static BOOLEAN
EmuIsochFbReap(
    _Inout_ PEMU_ISOCH_STREAM  S,
    _Inout_ PEMU_ISOCH_FB_SLOT Fb
    )
{
    struct _URB_ISOCH_TRANSFER* u = &Fb->Urb->UrbIsochronousTransfer;

    InterlockedIncrement(&S->Completes);
    if (!NT_SUCCESS((NTSTATUS)u->Hdr.Status) ||
        !USBD_SUCCESS(u->IsoPacket[0].Status)) {
        InterlockedIncrement(&S->IsoPacketErrors);
        InterlockedExchange(&S->LastPacketStatus,
                            (LONG)u->IsoPacket[0].Status);
    }
    InterlockedAdd(&S->Bytes, (LONG)u->IsoPacket[0].Length);
    if (InterlockedExchange(&Fb->State, EMU_ISOCH_SLOT_FREE) !=
        EMU_ISOCH_SLOT_ARMED) {
        return FALSE;
    }
    EmuIsochReleasePending(S);
    return TRUE;
}

// The feedback-URB completion: reap, account the error streak, and schedule
// the re-arm on the slot's timer — never on this stack. The dump of the 0x133
// shows why: the USB3 stack can complete the resubmitted URB inside the SAME
// interrupter DPC this completion runs in (Isoch_MapTransfers fires within
// the one DPC), so a direct resubmit cycles submit -> complete -> reap ->
// resubmit forever in a single DPC and every per-chain depth guard resets
// each lap. A 1 ms timer DPC carries at most one feedback submission; at the
// normal 32 ms feedback cadence the deferral is noise.
static NTSTATUS NTAPI
EmuIsochFbCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp,
    _In_opt_ PVOID      Context
    )
{
    PEMU_ISOCH_FB_SLOT fb = (PEMU_ISOCH_FB_SLOT)Context;
    PEMU_ISOCH_STREAM S = fb->Stream;
    struct _URB_ISOCH_TRANSFER* u = &fb->Urb->UrbIsochronousTransfer;
    NTSTATUS urbStatus;
    BOOLEAN fbOk;
    LARGE_INTEGER due;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    // The verdict is read before anything can rebuild the slot: this stack is
    // the only context that can, and it has not set the timer yet.
    urbStatus = (NTSTATUS)u->Hdr.Status;
    fbOk = NT_SUCCESS(urbStatus) && USBD_SUCCESS(u->IsoPacket[0].Status);

    if (EmuIsochFbReap(S, fb)) {
        if (fbOk) {
            InterlockedExchange(&S->FbConsecErrors, 0);
        } else if (InterlockedIncrement(&S->FbConsecErrors) >=
                   EMU_ISOCH_FB_FAIL_LIMIT) {
            // The old code re-armed on error unconditionally: a deterministically
            // broken endpoint fed itself forever (each cycle also marched the
            // cursor +32 while the bus advanced ~0 — the 66-hour StartFrame of
            // the dump). Bounded fail-stop: 4 consecutive errors end it.
            EmuIsochFailStop(S, EMU_ISOCH_FAIL_FB,
                             NT_SUCCESS(urbStatus)
                                 ? (NTSTATUS)u->IsoPacket[0].Status
                                 : urbStatus);
        }
        if (S->Quit == 0) {
            due.QuadPart = -1 * 10000LL;   // 1 ms
            KeSetTimer(&fb->Timer, due, &fb->TimerDpc);
        }
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// The deferred feedback re-arm: runs in its own DPC, one submission, done.
static VOID
EmuIsochFbTimerDpc(
    _In_ PKDPC  Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2
    )
{
    PEMU_ISOCH_FB_SLOT fb = (PEMU_ISOCH_FB_SLOT)DeferredContext;
    PEMU_ISOCH_STREAM S = fb->Stream;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    EmuIsochFeedbackRearm(S, (PEMU_DEVICE_CONTEXT)S->Owner, fb);
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
            // The diag record reads these after the completion: the planned
            // shape of the URB as it left the builder.
            Slot->Plan[p].Offset = off;
            Slot->Plan[p].Length = got * S->BytesPerFrame;
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
    // buffer is pre-filled once and repeats every pipeline cycle. The layout
    // comes from the tested planner, byte-identical to the inline version it
    // replaced, so the P1 wire behavior does not move under the diagnostics.
    {
        EMU_ISOCH_PACKET_PLAN plan[EMU_ISOCH_PKTS_PER_URB];
        ULONG ttl;

        if (!EmuIsochPlanFixedNominal(S->NominalFrames, S->BytesPerFrame,
                                      S->MaxPacketSize, Out, &ttl, plan)) {
            return FALSE;
        }
        u->TransferBufferLength = ttl;
        for (p = 0; p < EMU_ISOCH_PKTS_PER_URB; p++) {
            u->IsoPacket[p].Offset = plan[p].Offset;
            u->IsoPacket[p].Length = plan[p].Length;
            u->IsoPacket[p].Status = 0;
            Slot->Plan[p] = plan[p];
        }
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
        LONG anchor;
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
        anchor = EmuIsochCursorAnchor(now, &S->CursorFrame);
        if (anchor != 0) {
            S->AnchorFires++;
            // anchor 2 is the lead-ceiling clamp: the cursor outran the bus.
            // Counted separately so a runaway shows in the registry even when
            // the stream otherwise looks healthy.
            if (anchor == 2) {
                S->ClampFires++;
            }
        }
        u = &slot->Urb->UrbIsochronousTransfer;
        u->StartFrame = S->CursorFrame;
        S->CursorFrame += 1;   // the vendor's +1 frame per URB (trace-verified)
        S->CursorLast = S->CursorFrame;

        IoReuseIrp(slot->Irp, STATUS_SUCCESS);
        stack = IoGetNextIrpStackLocation(slot->Irp);
        stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
        stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
        USBD_AssignUrbToIoStackLocation(Ctx->UsbdHandle, stack, slot->Urb);
        IoSetCompletionRoutine(slot->Irp, EmuIsochDataCompletion, slot,
                               TRUE, TRUE, TRUE);

        slot->State = EMU_ISOCH_SLOT_ARMED;
        if (InterlockedCompareExchange(&S->Pending, 0, 0) == 0) {
            // The Idle latch, cleared whenever Pending is taken 0 -> 1. A
            // notification event that is never cleared latches its first
            // transient drain-to-zero forever, and the stop drain then
            // returns instantly and retains the stream as "not drained" —
            // the old behavior.
            KeClearEvent(&S->Idle);
        }
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
            if (!NT_SUCCESS(status)) {
                if (InterlockedCompareExchange(&S->LastUrbStatus, 0, 0) !=
                    (LONG)status) {
                    InterlockedIncrement(&S->UrbFailures);
                    InterlockedExchange(&S->LastUrbStatus, (LONG)status);
                }
                // A deterministic rejection feeds itself: each inline
                // completion frees the slot and this loop arms again. The
                // spin bound below caps one pump pass; this counter caps the
                // stream — after EMU_ISOCH_SUBMIT_FAIL_LIMIT consecutive
                // rejected submissions it fail-stops instead of handing the
                // storm to the next pump event (the IOCTL-code bug class:
                // every submit failed, nothing bounded the retrying).
                if (InterlockedIncrement(&S->SubmitConsecErrors) >=
                    EMU_ISOCH_SUBMIT_FAIL_LIMIT) {
                    EmuIsochFailStop(S, EMU_ISOCH_FAIL_SUBMIT, status);
                }
            } else {
                InterlockedExchange(&S->SubmitConsecErrors, 0);
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
            InterlockedExchange(&S->SubmitConsecErrors, 0);
        }
        // Loop: re-acquire at the top and keep draining the ring.
    }
    InterlockedExchange(&S->Pumping, 0);
}

// Arm one feedback URB: two slots keep the endpoint serviced, consecutive
// StartFrames exactly EMU_ISOCH_FB_STEP (32) apart — the trace's feedback
// delta — and always in the future. The cursor re-anchors whenever it fell
// into the past (the vendor's sync builder, FUN_00013E98 @0x3F1B) and is
// clamped when it leads the bus by more than EMU_ISOCH_MAX_LEAD (the 0x133
// runaway: each storm cycle marched the cursor +32 while the bus advanced
// ~0). The submit happens outside the pump lock, like the data pump. Called
// from the start path and from the slot's 1 ms timer DPC — never from inside
// the completion.
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
    LONG anchor;
    NTSTATUS status;

    KeAcquireSpinLock(&S->PumpLock, &oldIrql);
    if (S->FeedbackPipe == NULL || !S->Active || S->Quit != 0 ||
        Fb->State != EMU_ISOCH_SLOT_FREE) {
        // The State check: the timer DPC can overlap a restart's initial arm
        // or fire after a stop's cancel; an ARMED slot must never be rebuilt
        // — that would IoReuseIrp an IRP still in flight.
        KeReleaseSpinLock(&S->PumpLock, oldIrql);
        return;
    }

    now = EmuIsochBusFrame(S);
    anchor = EmuIsochCursorAnchor(now, &S->FbCursorFrame);
    if (anchor != 0) {
        S->AnchorFires++;
        if (anchor == 2) {
            S->ClampFires++;
        }
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
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    USBD_AssignUrbToIoStackLocation(Ctx->UsbdHandle, stack, Fb->Urb);
    IoSetCompletionRoutine(Fb->Irp, EmuIsochFbCompletion, Fb, TRUE, TRUE, TRUE);

    Fb->State = EMU_ISOCH_SLOT_ARMED;
    if (InterlockedCompareExchange(&S->Pending, 0, 0) == 0) {
        // The Idle latch, cleared on every 0 -> 1 step (see the pump).
        KeClearEvent(&S->Idle);
    }
    InterlockedIncrement(&S->Pending);
    InterlockedIncrement(&S->Submits);
    KeReleaseSpinLock(&S->PumpLock, oldIrql);

    status = IoCallDriver(Ctx->Pdo, Fb->Irp);
    if (status != STATUS_PENDING) {
        // Completed inline: the completion reaped and scheduled the timer
        // already, on this same thread. The slot lifecycle stays the
        // completion's — see the data pump for the contract analysis.
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
    S->FailStage = EMU_ISOCH_FAIL_NONE;
    S->FailStatus = 0;
    S->SubmitConsecErrors = 0;
    S->UrbConsecErrors = 0;
    S->FbConsecErrors = 0;
    S->FqInline = 0;
    S->FqFloat = 0;
    S->FqFail = 0;
    S->FqBusy = 0;
    S->AnchorFires = 0;
    S->ClampFires = 0;
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
    ULONG                     i;
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
    // frame-query outcomes, the anchor's view, the fail-stop verdict and why
    // the pump stopped.
    {
        EmuNoteStatusValue(Ctx, Direction == EMU_STREAM_RENDER
                                ? L"PumpUnderruns" : L"PumpUnderrunsC",
                           (ULONG)S->Underruns);
        EmuNoteStatusValue(Ctx, L"FqOk", (ULONG)S->FqInline);
        EmuNoteStatusValue(Ctx, L"FqFloat", (ULONG)S->FqFloat);
        EmuNoteStatusValue(Ctx, L"FqFail", (ULONG)S->FqFail);
        EmuNoteStatusValue(Ctx, L"FqBusy", (ULONG)S->FqBusy);
        EmuNoteStatusValue(Ctx, L"AnchorFires", (ULONG)S->AnchorFires);
        EmuNoteStatusValue(Ctx, L"ClampFires", (ULONG)S->ClampFires);
        EmuNoteStatusValue(Ctx, L"FailStage", (ULONG)S->FailStage);
        EmuNoteStatusValue(Ctx, L"FailStatus", (ULONG)S->FailStatus);
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
            // referenced by its IRP — except on the retained-timeout path
            // (STATUS_CANCELLED): the stack still owns the flight and its
            // completion will still write the URB status, so the URB is
            // retained alongside the IRP. A deliberate leak on a black-holed
            // device beats freeing memory the stack can still touch.
            if (status != STATUS_CANCELLED) {
                USBD_UrbFree(Ctx->UsbdHandle, abortUrb);
            }
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
            status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, abortUrb);
            if (status != STATUS_CANCELLED) {
                USBD_UrbFree(Ctx->UsbdHandle, abortUrb);
            }
        }
    }

    // Cancel what the aborts cannot reach. The pipe aborts flush everything
    // queued on them, but a contract-broken stack can return non-pending
    // without running the completion (the wedged-ARMED class), and the frame
    // query lives on no pipe at all — a floated query kept Pending raised
    // until a completion that might never come. IoCancelIrp on the owned
    // IRPs turns every such wedge into a completion with STATUS_CANCELLED;
    // the drain below then actually drains. Cancelled feedback completions
    // schedule no new re-arm (the completion checks Quit), and both timers
    // are disarmed so no deferred re-arm wakes up mid-drain.
    KeAcquireSpinLock(&S->PumpLock, &oldIrql);
    for (i = 0; i < EMU_ISOCH_SLOTS; i++) {
        if (S->Slots[i].State == EMU_ISOCH_SLOT_ARMED &&
            S->Slots[i].Irp != NULL) {
            IoCancelIrp(S->Slots[i].Irp);
        }
    }
    for (i = 0; i < EMU_ISOCH_FB_SLOTS; i++) {
        KeCancelTimer(&S->Fb[i].Timer);
        if (S->Fb[i].State == EMU_ISOCH_SLOT_ARMED &&
            S->Fb[i].Irp != NULL) {
            IoCancelIrp(S->Fb[i].Irp);
        }
    }
    KeReleaseSpinLock(&S->PumpLock, oldIrql);
    if (S->FrameIrp != NULL &&
        InterlockedCompareExchange(&S->FrameBusy, 0, 0) != 0) {
        IoCancelIrp(S->FrameIrp);
    }

    // Drain: after the aborts and cancels every armed URB completes; each
    // reap drains Pending and the last one sets Idle. The event is cleared
    // first — a stale signal from a transient drain during streaming would
    // otherwise make every wait below return instantly. The timeout is a
    // tripwire only - a stream that outlives it is retained, never freed.
    KeClearEvent(&S->Idle);
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

    // Close the completion-tail window: Pending can reach zero while the
    // last completion is still running its pump tail, or while the frame
    // query's completion is between the drain and the event. Both flags
    // clear within one completion; the bound exists so a wedged completion
    // cannot hang the stop path (FreeStream's Pending re-check is still the
    // load-bearing tripwire).
    wait.QuadPart = -1 * 10000LL;   // 1 ms
    for (waits = 0; waits < 100; waits++) {
        if (InterlockedCompareExchange(&S->Pumping, 0, 0) == 0 &&
            InterlockedCompareExchange(&S->FrameBusy, 0, 0) == 0) {
            break;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
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
    // 24-frame packets, which requires the data endpoint to service every
    // 500 us (raw bInterval 3 = 4 microframes; the vendor's packets land on
    // microframes 0 and 4 of each 1 ms frame — the trace). The alt selection
    // (EmuIsochFindEngineAlt) picks exactly that alt. USBD
    // PipeInformation.Interval reads the RAW descriptor bInterval — the old
    // "reads 4 = microframes per packet" note here was a misreading that had
    // the engine arm the bInterval-4 alt, and on the wire that made the
    // capture IN URBs complete with CRC/babble and half the render packets
    // with ISO_TD_ERROR (hardware session 2026-11-07). The feedback endpoint
    // reports 24 frames per 500-us window (16.16 value 0x00180000) and is
    // serviced by two async URBs.
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
    // Which alternate the stream actually runs on. The bInterval question
    // (raw descriptor says 4 = 1 ms service interval on the data endpoint,
    // while the vendor trace says two packets per 1 ms at microframes 0 and
    // 4, i.e. a 500 us interval) is settled by the wire, not by the fixture:
    // this value plus TestPipe*/PipeUs* pin the choice for the next trace
    // comparison. High byte interface, low byte alternate.
    EmuNoteStatusValue(Ctx, Direction == EMU_STREAM_RENDER
                            ? L"AltSelR" : L"AltSelC",
                       (Alt->InterfaceNumber << 8) | (Alt->AltSetting & 0xFF));

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
        KeInitializeTimer(&fb->Timer);
        KeInitializeDpc(&fb->TimerDpc, EmuIsochFbTimerDpc, fb);
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
    S->FailStage = EMU_ISOCH_FAIL_NONE;
    S->FailStatus = 0;
    S->SubmitConsecErrors = 0;
    S->UrbConsecErrors = 0;
    S->FbConsecErrors = 0;
    S->FqInline = 0;
    S->FqFloat = 0;
    S->FqFail = 0;
    S->FqBusy = 0;
    S->AnchorFires = 0;
    S->ClampFires = 0;
    S->NowLast = 0;
    S->CursorLast = 0;
    S->PumpQuitExit = 0;
    S->PumpPoolExit = 0;
    S->PumpGateExit = 0;
    S->PumpBuildExit = 0;
    // The P1 wire history starts empty per run: rows keep the previous
    // session's data until the next start, so a stop - planned or failed -
    // can be read afterwards, but no session ever sees another's rows.
    for (i = 0; i < EMU_ISOCH_DIAG_ROWS; i++) {
        S->Diag[i].Seq = 0;
    }
    S->DiagSeq = 0;

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
            ULONG units = delta / 10;

            // The raw value lands in the registry even when it is refused:
            // the next diagnosis should not have to guess what the device
            // actually reported.
            EmuNoteStatusValue(Ctx, Direction == EMU_STREAM_RENDER
                                    ? L"CalFramesPerMs" : L"CalFramesPerMsC",
                               units);
            if (!EmuIsochFramesPerMsValid(units)) {
                // The mapping scales every anchor with this number; garbage
                // units schedule every packet into the past or the far
                // future — the runaway class. The old code silently mapped 0
                // to 1; a frozen or foreign counter now fails the start
                // loudly instead.
                EMU_LOG_ERROR(STR_MODULENAME "dir %lu: frame counter units %lu "
                              "over a 10 ms window are not schedulable\n",
                              Direction, units);
                EmuIsochFreeStream(Ctx, Direction);
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            S->FramesPerMs = units;
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
            status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, resetUrb);
            if (S->FeedbackPipe != NULL) {
                resetUrb->UrbPipeRequest.PipeHandle = S->FeedbackPipe;
                if (EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, resetUrb) ==
                    STATUS_CANCELLED) {
                    status = STATUS_CANCELLED;
                }
            }
            // Retained-timeout contract: on STATUS_CANCELLED the stack still
            // owns the flight and its completion will still write the URB
            // status — the URB is retained with the IRP (see
            // EmuUsbSubmitUrb), not freed under it.
            if (status != STATUS_CANCELLED) {
                USBD_UrbFree(Ctx->UsbdHandle, resetUrb);
            }
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

// The engine's alternate pick: the stream facts of EmuCapsFindAlt plus the
// wire-grid preference (EMU_ISOCH_DATA_BINTERVAL). Falls back to the plain
// descriptor-order match for devices that list no 500 us alt.
static const EMU_STREAM_ALT*
EmuIsochFindEngineAlt(
    _In_ const EMU_DEVICE_CAPS* Caps,
    _In_ ULONG Direction,
    _In_ ULONG Channels,
    _In_ ULONG RateHz
    )
{
    ULONG i = EmuIsochPickAltIndex(Caps->StreamAlts, Caps->StreamAltCount,
                                   Direction, Channels, RateHz);

    return i < Caps->StreamAltCount
        ? &Caps->StreamAlts[i]
        : EmuCapsFindAlt(Caps, Direction, Channels, RateHz);
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
    alt = EmuIsochFindEngineAlt(Ctx->Caps, EMU_STREAM_RENDER, 2, RateHz);
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
    alt[0] = EmuIsochFindEngineAlt(Ctx->Caps, EMU_STREAM_RENDER, 2, RateHz);
    alt[1] = EmuIsochFindEngineAlt(Ctx->Caps, EMU_STREAM_CAPTURE, 2, RateHz);
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
// The diag reply: both engines' wire history, oldest live entry first. Rows
// the ring has already turned over since a snapshot was requested are simply
// absent (their Seq shows the newest epoch instead of the expected one); a
// row captured mid-write is skipped rather than published torn. No lock: the
// completion routines own their rows until 32 further completions have passed.
VOID
EmuIsochFillDiag(
    _Out_ PEMU_ISOCH_DIAG_INFO Out,
    _In_ PEMU_DEVICE_CONTEXT     Ctx
    )
{
    PAGED_CODE();

    RtlZeroMemory(Out, sizeof(*Out));
    Out->Size = sizeof(EMU_ISOCH_DIAG_INFO);
    Out->Version = EMU_IOCTL_VERSION;

    for (ULONG d = 0; d < 2; d++) {
        PEMU_ISOCH_STREAM        S = &Ctx->Isoch[d];
        PEMU_ISOCH_DIAG_STREAM   dst = &Out->Stream[d];
        // A negative counter means the 2^31-completion wrap (years of a
        // stressed run); the snapshot answers empty rather than walking a
        // wrapped sequence backwards.
        LONG  last = InterlockedCompareExchange(&S->DiagSeq, 0, 0);
        ULONG count = last > 0 ? (ULONG)last : 0;
        ULONG first = count > EMU_ISOCH_DIAG_ROWS
                          ? count - EMU_ISOCH_DIAG_ROWS + 1
                          : 1;

        dst->TotalSeq = count;
        for (ULONG s = first; s <= count; s++) {
            PEMU_ISOCH_DIAG_ROW src = &S->Diag[(s - 1) % EMU_ISOCH_DIAG_ROWS];
            PEMU_ISOCH_DIAG_ROW cell = &dst->Row[s - first];

            if (src->Seq == (LONG)s) {
                *cell = *src;
            }
            // A mismatching Seq is a row from an older or a not-yet-written
            // epoch; leaving it zeroed says "no sample", never garbage.
        }
    }
}
#pragma code_seg()

#endif // EMU_KERNEL
