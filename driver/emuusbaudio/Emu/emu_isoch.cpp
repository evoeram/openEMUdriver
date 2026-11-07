// EmuUSBAudio — isochronous stream engine, M3 stage B (test slice).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// One direction = one system thread = EMU_ISOCH_SLOTS self-rearming URBs, one
// packet per URB, explicit StartFrame advancing one period per submission.
// The completion routine only signals an event; everything else (harvest,
// counters, resubmit) happens in the thread at PASSIVE_LEVEL, which keeps the
// DISPATCH_LEVEL side of the engine down to what the USB stack does anyway.
//
// The stop path is the load-bearing part: set Quit, abort the pipe so every
// queued URB completes, wait for the thread (which drains every submitted
// slot before exiting), and only then free. Nothing is freed while an IRP of
// this engine can still be running — the plan's rundown rule, obeyed literally.
// A slot whose final completion never arrives is deliberately leaked rather
// than freed; the alternative is freeing memory an IRP still references.
#include "emu_isoch.h"
#include <ntstrsafe.h>

// --- pure scheduling helpers -------------------------------------------------
// (nonpaged in the image: the worker calls them per submission and the start
// path pages in whatever it needs around them)

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

// The URB completion routine: signal the slot's event and keep the IRP. Iso
// completions arrive at DISPATCH_LEVEL, which is exactly why it does nothing
// else. The routine is registered afresh for every submission.
static NTSTATUS NTAPI
EmuIsochCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp,
    _In_opt_ PVOID      Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    if (Context != NULL) {
        KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// One URB submission on a slot's reused IRP. Reinitialize the IRP, clear the
// previous completion event and re-arm the stack location on every submission.
static NTSTATUS
EmuIsochSubmitSlot(
    _Inout_ PEMU_ISOCH_STREAM      S,
    _In_ PEMU_DEVICE_CONTEXT      Ctx,
    _In_ ULONG                    Index
    )
{
    PEMU_ISOCH_SLOT    slot = &S->Slots[Index];
    PIO_STACK_LOCATION stack;
    NTSTATUS           status;

    // Wall-clock pacing, no future scheduling: this USB stack never completes
    // an iso URB whose StartFrame lies ahead (2.0.0.54 held the resubmissions
    // until abort; only the prime's four packets ever completed) and burns
    // through past-due ones instantly (2.0.0.51/52). The worker therefore
    // submits each packet right at its scheduled slot - instant completion
    // is expected - and the exact 100-ns wait below is what paces the wire to
    // one packet per PeriodUs microseconds.
    {
        ULONGLONG nsPerPacket = (ULONGLONG)S->PeriodUs * 10;
        ULONGLONG due = S->PaceOrigin +
            (slot->NextFrame - S->PaceBaseFrame) * nsPerPacket;
        ULONGLONG now = KeQueryInterruptTime();

        if (now < due) {
            LARGE_INTEGER delay;

            delay.QuadPart = -(LONGLONG)(due - now);
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
    }

    IoReuseIrp(slot->Irp, STATUS_SUCCESS);
    KeClearEvent(&slot->Done);
    stack = IoGetNextIrpStackLocation(slot->Irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    // AssignUrbToIoStackLocation writes both the URB pointer and the
    // IOCTL_INTERNAL_USB_SUBMIT_URB control code into the location.
    USBD_AssignUrbToIoStackLocation(Ctx->UsbdHandle, stack, slot->Urb);
    // The classic (non-Ex) registration: the routine only signals the slot's
    // event, which is legal at any IRQL, and the worker always waits for it.
    // The Ex variant queues a special APC for PASSIVE execution, and when the
    // USB stack completes a failed URB synchronously inside IoCallDriver that
    // APC insert bugchecked (the 2.0.0.61 attach crash,
    // KeInsertQueueApc under IopfCompleteRequest).
    IoSetCompletionRoutine(slot->Irp, EmuIsochCompletion, &slot->Done,
                           TRUE, TRUE, TRUE);
    status = STATUS_SUCCESS;

    slot->Submitted = TRUE;
    InterlockedIncrement(&S->Submits);
    status = IoCallDriver(Ctx->Pdo, slot->Irp);
    if (!NT_SUCCESS(status)) {
        // Failed synchronously: the completion will not run for this submit,
        // so the slot is not in flight after all.
        slot->Submitted = FALSE;
        InterlockedIncrement(&S->UrbFailures);
        InterlockedExchange(&S->LastUrbStatus, (LONG)status);
    }
    return status;
}

// Per-submit URB fields: everything but the slot buffer address, which never
// changes. WaveRT data is packed immediately before every submit, prime included.
static NTSTATUS
EmuIsochArmUrb(
    _Inout_ PEMU_ISOCH_STREAM S,
    _Inout_ PEMU_ISOCH_SLOT   Slot,
    _In_ BOOLEAN              Out
    )
{
    struct _URB_ISOCH_TRANSFER* u = &Slot->Urb->UrbIsochronousTransfer;
    ULONG packetBytes = S->NominalFrames * S->BytesPerFrame;
    PUCHAR transferBuffer = S->Buffer +
        (ULONG)(Slot - &S->Slots[0]) * S->SlotBytes;
    NTSTATUS status = STATUS_SUCCESS;

    if (Out && S->FillRender != NULL) {
        status = S->FillRender(S->CallbackContext, transferBuffer,
                               S->NominalFrames,
                               S->BaseFrames + (ULONGLONG)S->SubmittedFrames);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        // Only the worker writes this counter. It describes data consumed into
        // USB slot buffers, never the externally visible playback position.
        S->SubmittedFrames += S->NominalFrames;
    }

    u->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    u->Hdr.Length = (USHORT)sizeof(struct _URB_ISOCH_TRANSFER);
    u->Hdr.Status = 0;
    u->PipeHandle = S->Pipe;
    u->TransferFlags = Out
        ? USBD_TRANSFER_DIRECTION_OUT
        : (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK);
    u->TransferBuffer = transferBuffer;
    u->TransferBufferLength = Out ? packetBytes : S->SlotBytes;
    // StartFrame = the current frame, computed from the calibrated pacing
    // anchors. This stack's iso scheduling is broken at the extremes: a
    // future StartFrame is held forever (2.0.0.54), and 2.0.0.61's StartFrame
    // 0 (forty-nine days in the past on the frame counter) bugchecked the
    // xHCI scheduling arithmetic on the first device attach. "Now" completes
    // immediately and the worker's wall-clock wait produces the wire pace.
    u->StartFrame = (ULONG)(S->PaceBaseFrame +
        (KeQueryInterruptTime() - S->PaceOrigin) / 10000);
    u->NumberOfPackets = 1;
    u->IsoPacket[0].Offset = 0;
    u->IsoPacket[0].Length = Out ? packetBytes : S->SlotBytes;
    u->IsoPacket[0].Status = 0;
    // The slot stride is the whole pipeline: NextFrame is a packet index, and
    // the four slots tile it without overlap only when each advances by the
    // slot count. The old M3-B stride (PeriodMs) equaled the slot count by
    // accident and 2.0.0.51's one-millisecond period made every slot claim
    // the same packets as its neighbour - the second URB for a packet window
    // is held by the controller forever and the worker froze after the prime.
    Slot->NextFrame += EMU_ISOCH_SLOTS;
    return STATUS_SUCCESS;
}

// The feedback reader: one 4-byte iso IN packet on the feedback endpoint,
// submitted ASAP and rearmed by the worker whenever it completes. The
// asynchronous OUT sink never locks its DAC unless this endpoint is being
// serviced - the 2.0.0.23..2.0.0.58 silence had every other wire parameter
// matching the vendor trace byte for byte. The returned value itself (the
// device's actual frame clock) is M5's pacing input.
static NTSTATUS
EmuIsochArmFeedback(
    _Inout_ PEMU_ISOCH_STREAM S
    )
{
    struct _URB_ISOCH_TRANSFER* u = &S->FbUrb->UrbIsochronousTransfer;

    u->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    u->Hdr.Length = (USHORT)sizeof(struct _URB_ISOCH_TRANSFER);
    u->Hdr.Status = 0;
    u->PipeHandle = S->FeedbackPipe;
    u->TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK;
    u->TransferBuffer = S->FbBuffer;
    u->TransferBufferLength = 4;
    // "Now", never zero: StartFrame 0 sits forty-nine days in the counter's
    // past and the xHCI scheduling arithmetic bugchecks on it (2.0.0.61).
    u->StartFrame = (ULONG)(S->PaceBaseFrame +
        (KeQueryInterruptTime() - S->PaceOrigin) / 10000);
    u->NumberOfPackets = 1;
    u->IsoPacket[0].Offset = 0;
    u->IsoPacket[0].Length = 4;
    u->IsoPacket[0].Status = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
EmuIsochSubmitFeedback(
    _Inout_ PEMU_ISOCH_STREAM   S,
    _In_ PEMU_DEVICE_CONTEXT    Ctx
    )
{
    // The feedback packet is submitted synchronously from the feedback
    // thread (EmuIsochFeedbackWorker): EmuUsbSubmitUrb allocates its own IRP
    // and waits, so no completion routine, no reuse race - the asynchronous
    // reader of 2.0.0.61/62 crashed inside the controller's completion APC
    // machinery.
    return EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, S->FbUrb);
}

// The feedback thread: submit one 4-byte iso IN packet, wait for the device
// to fill it, repeat. The device's own feedback interval paces the loop
// (~30 packets per second on the 0404); StopStream aborts the feedback
// pipe, which completes the pending packet and lets the loop see Quit.
static VOID
EmuIsochFeedbackWorker(
    _In_ PVOID StartContext
    )
{
    PEMU_ISOCH_STREAM   S = (PEMU_ISOCH_STREAM)StartContext;
    PEMU_DEVICE_CONTEXT Ctx = (PEMU_DEVICE_CONTEXT)S->Owner;

    while (S->FbUrb != NULL &&
           InterlockedCompareExchange(&S->Quit, 0, 0) == 0) {
        if (!NT_SUCCESS(EmuIsochArmFeedback(S))) {
            break;
        }
        if (!NT_SUCCESS(EmuIsochSubmitFeedback(S, Ctx))) {
            break;
        }
    }
}

// Read one completion into the counters. A render frame becomes visible to
// WaveRT only after its complete packet succeeded on the wire; cancelled,
// failed and short packets remain visible in the error counters instead.
static VOID
EmuIsochHarvest(
    _Inout_ PEMU_ISOCH_STREAM S,
    _Inout_ PEMU_ISOCH_SLOT   Slot,
    _In_ BOOLEAN              Out
    )
{
    struct _URB_ISOCH_TRANSFER* u = &Slot->Urb->UrbIsochronousTransfer;
    ULONG packetBytes = S->NominalFrames * S->BytesPerFrame;
    ULONG got;
    BOOLEAN irpSuccess = NT_SUCCESS(Slot->Irp->IoStatus.Status);
    BOOLEAN urbSuccess = NT_SUCCESS((NTSTATUS)u->Hdr.Status);
    BOOLEAN packetSuccess = USBD_SUCCESS(u->IsoPacket[0].Status);

    Slot->Submitted = FALSE;
    InterlockedIncrement(&S->Completes);

    if (!irpSuccess || !urbSuccess) {
        InterlockedIncrement(&S->UrbFailures);
        InterlockedExchange(&S->LastUrbStatus,
                            !irpSuccess ? (LONG)Slot->Irp->IoStatus.Status
                                        : (LONG)u->Hdr.Status);
    }
    if (!packetSuccess) {
        InterlockedIncrement(&S->IsoPacketErrors);
        InterlockedExchange(&S->LastPacketStatus,
                            (LONG)u->IsoPacket[0].Status);
    }
    got = u->IsoPacket[0].Length;
    if (got < packetBytes) {
        InterlockedIncrement(&S->ShortPackets);
    }
    InterlockedAdd(&S->Bytes, (LONG)got);

    if (Out && irpSuccess && urbSuccess && packetSuccess &&
        got == packetBytes) {
        S->CompletedFrames += S->NominalFrames;
        if (S->RenderCompleted != NULL) {
            S->RenderCompleted(S->CallbackContext,
                               (ULONGLONG)S->CompletedFrames);
        }
    }
}

// The OUT pattern: a 24-bit counter ramp, both channels carrying the same
// value. Deterministic, recognizable on a wiretap, no floating point. The M4
// WaveRT path replaces this with client data; the M3 criterion's "sine on
// OUT" is judged by real audio flowing, which needs the client side first.
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

// The per-direction worker: prime every slot, then harvest-and-resubmit in
// submission order until Quit. Runs at PASSIVE_LEVEL in the system process;
// everything it touches is nonpaged. The final drain waits for every slot it
// may have left in flight - after the stop path's abort that wait is short,
// and a slot that outlives the timeout is deliberately left unreleased.
static VOID
EmuIsochWorker(
    _In_ PVOID StartContext
    )
{
    PEMU_ISOCH_STREAM      S = (PEMU_ISOCH_STREAM)StartContext;
    PEMU_DEVICE_CONTEXT    Ctx = (PEMU_DEVICE_CONTEXT)S->Owner;
    BOOLEAN                out = (S->Direction == EMU_STREAM_RENDER);
    LARGE_INTEGER          drainTimeout;
    NTSTATUS               status;

    drainTimeout.QuadPart = -2 * 1000 * 10000LL;   // 2 s, tripwire only

    for (ULONG i = 0; i < EMU_ISOCH_SLOTS; i++) {
        status = EmuIsochArmUrb(S, &S->Slots[i], out);
        if (NT_SUCCESS(status)) {
            status = EmuIsochSubmitSlot(S, Ctx, i);
        }
        if (!NT_SUCCESS(status)) {
            EMU_LOG_ERROR(STR_MODULENAME "isoch slot %lu prime failed, 0x%x\n",
                          (ULONG)i, status);
        }
    }
    // The feedback reader lives on its own thread (EmuIsochFeedbackWorker):
    // a synchronous reader here would block the data pipeline, and the
    // asynchronous shared-IRP reader of 2.0.0.61/62 crashed in the
    // controller's completion machinery.

    while (InterlockedCompareExchange(&S->Quit, 0, 0) == 0) {
        BOOLEAN anySubmitted = FALSE;

        for (ULONG i = 0; i < EMU_ISOCH_SLOTS; i++) {
            PEMU_ISOCH_SLOT slot = &S->Slots[i];

            if (!slot->Submitted) {
                continue;
            }
            anySubmitted = TRUE;
            KeWaitForSingleObject(&slot->Done, Executive, KernelMode, FALSE,
                                  NULL);
            EmuIsochHarvest(S, slot, out);
            if (InterlockedCompareExchange(&S->Quit, 0, 0) != 0) {
                break;
            }
            status = EmuIsochArmUrb(S, slot, out);
            if (NT_SUCCESS(status)) {
                status = EmuIsochSubmitSlot(S, Ctx, i);
            }
            if (!NT_SUCCESS(status)) {
                EMU_LOG_ERROR(STR_MODULENAME "isoch slot %lu resubmit failed, "
                              "0x%x\n", (ULONG)i, status);
            }
        }
        if (!anySubmitted &&
            InterlockedCompareExchange(&S->Quit, 0, 0) == 0) {
            LARGE_INTEGER retryDelay;
            retryDelay.QuadPart = -10000LL; // 1 ms; no failed-submit busy spin
            KeDelayExecutionThread(KernelMode, FALSE, &retryDelay);
        }
    }

    // Drain: after the abort every queued URB completes; the timeout exists
    // so a stuck slot is counted, not waited on forever.
    for (ULONG i = 0; i < EMU_ISOCH_SLOTS; i++) {
        PEMU_ISOCH_SLOT slot = &S->Slots[i];

        if (!slot->Submitted) {
            continue;
        }
        status = KeWaitForSingleObject(&slot->Done, Executive, KernelMode,
                                       FALSE, &drainTimeout);
        if (status == STATUS_SUCCESS) {
            EmuIsochHarvest(S, slot, out);
        } else {
            // The slot keeps Submitted = TRUE, which is exactly the marker
            // the stop path uses to skip freeing an IRP still in flight.
            EMU_LOG_ERROR(STR_MODULENAME "isoch slot %lu did not drain, "
                          "0x%x\n", (ULONG)i, status);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// A slot still marked Submitted may reference any other slot's URB through
// the shared USBD handle, and always references the stream's common buffer.
// In that case retain the whole allocation set: freeing only the quiet slots
// would be just as unsafe as freeing the stuck one.
#pragma code_seg("PAGE")
static BOOLEAN
EmuIsochHasOutstandingSlots(
    _In_ PEMU_ISOCH_STREAM S
    )
{
    PAGED_CODE();

    for (ULONG i = 0; i < EMU_ISOCH_SLOTS; i++) {
        if (S->Slots[i].Submitted) {
            return TRUE;
        }
    }
    return FALSE;
}

// Free what a stream owns. Every normal call site has already stopped the
// thread and drained its IRPs. A stuck completion retains the entire stream;
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

    PAGED_CODE();

    if (S->ThreadHandle != NULL) {
        // Reached only when ObReferenceObjectByHandle failed; StopStream has
        // already waited on this handle before any slot storage is released.
        ZwClose(S->ThreadHandle);
        S->ThreadHandle = NULL;
    }

    if (EmuIsochHasOutstandingSlots(S)) {
        EMU_LOG_ERROR(STR_MODULENAME "isoch dir %lu retained: completion still "
                      "in flight at free\n", Direction);
        S->Active = FALSE;
        return;
    }

    for (ULONG i = 0; i < EMU_ISOCH_SLOTS; i++) {
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
    if (S->Buffer != NULL) {
        ExFreePoolWithTag(S->Buffer, EMU_POOLTAG);
        S->Buffer = NULL;
    }
    // The feedback reader resources: the threads have been waited on, so
    // nothing is in flight here anymore.
    if (S->FbUrb != NULL) {
        USBD_UrbFree(Ctx->UsbdHandle, S->FbUrb);
        S->FbUrb = NULL;
    }
    if (S->FbBuffer != NULL) {
        ExFreePoolWithTag(S->FbBuffer, EMU_POOLTAG);
        S->FbBuffer = NULL;
    }
    S->Active = FALSE;
    S->Quit = 0;
    S->Pipe = NULL;
    S->CallbackContext = NULL;
    S->FillRender = NULL;
    S->RenderCompleted = NULL;
    S->BaseFrames = 0;
    S->SubmittedFrames = 0;
    // CompletedFrames survives until the next start: WaveRT reads it after the
    // worker has drained to preserve RUN -> PAUSE -> RUN positioning.
}

// Stop one direction: quit flag, pipe abort, thread drain, then free. The
// abort is a bus operation and belongs to a live device; on the release path
// (device possibly gone) it fails and the drain still runs - a canceled or
// dead URB completes either way.
static VOID
EmuIsochStopStream(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ ULONG                  Direction
    )
{
    PEMU_ISOCH_STREAM         S = &Ctx->Isoch[Direction];
    PURB                      abortUrb = NULL;
    NTSTATUS                  status;

    PAGED_CODE();

    if (!S->Active) {
        return;
    }
    InterlockedExchange(&S->Quit, 1);

    if (S->Pipe != NULL) {
        // Contract-602 tracks URB allocations in the USBD handle. A stack URB
        // is rejected by USB3 with bugcheck 0x144/3 before the request runs.
        status = USBD_UrbAllocate(Ctx->UsbdHandle, &abortUrb);
        if (NT_SUCCESS(status)) {
            // Abort every queued transfer on the data pipe: the worker's drain
            // waits depend on each of them completing, canceled or not.
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
        // Abort the feedback pipe BEFORE waiting on the threads: the
        // feedback thread is parked inside a synchronous EmuUsbSubmitUrb on
        // that pipe, and the abort is what completes it.
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

    if (S->ThreadObject != NULL) {
        KeWaitForSingleObject(S->ThreadObject, Executive, KernelMode, FALSE,
                              NULL);
        ObDereferenceObject(S->ThreadObject);
        S->ThreadObject = NULL;
    } else if (S->ThreadHandle != NULL) {
        KeWaitForSingleObject(S->ThreadHandle, Executive, KernelMode, FALSE,
                              NULL);
    }
    if (S->FbThreadObject != NULL) {
        KeWaitForSingleObject(S->FbThreadObject, Executive, KernelMode, FALSE,
                              NULL);
        ObDereferenceObject(S->FbThreadObject);
        S->FbThreadObject = NULL;
    } else if (S->FbThreadHandle != NULL) {
        KeWaitForSingleObject(S->FbThreadHandle, Executive, KernelMode, FALSE,
                              NULL);
        ZwClose(S->FbThreadHandle);
        S->FbThreadHandle = NULL;
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
// submitted select-interface URB, build the slots, start the worker.
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
    PURB                         frameUrb = NULL;
    OBJECT_ATTRIBUTES            oa;
    HANDLE                       threadHandle;
    ULONG                        base;
    BOOLEAN                      out = (Direction == EMU_STREAM_RENDER);
    NTSTATUS                     status;

    PAGED_CODE();

    if (S->Active || S->ThreadObject != NULL || S->ThreadHandle != NULL ||
        EmuIsochHasOutstandingSlots(S)) {
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
    // endpoint, parked here for M5 rather than streamed.
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
    // The wire grid follows the device's own bInterval: the 0404's streaming
    // alternates carry bInterval 4 (high-speed microframes) = one iso
    // transaction every 500 us, and the feedback endpoint reports 24 frames
    // per that window (16.16 value 0x00180000 in the vendor trace). The CA0189
    // hardware iso mechanism consumes FIFO on that schedule - 48-frame packets
    // once per millisecond starve it (every earlier build, silent). The
    // feedback thread services the endpoint continuously (2.0.0.65 sync
    // reader, safe).
    S->PeriodUs = S->PipeInterval * 125;
    if (S->PeriodUs == 0 || S->PeriodUs > 1000) {
        S->PeriodUs = 500;
    }
    S->FramesPerPacket = RateHz * S->PeriodUs / 1000000;
    if (S->FramesPerPacket == 0) {
        S->FramesPerPacket = 1;
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
        // WaveRT slots are filled afresh by the prime pass; capture and any
        // not-yet-submitted headroom begin as silence rather than stale data.
        RtlZeroMemory(S->Buffer, (SIZE_T)EMU_ISOCH_SLOTS * S->SlotBytes);
    }

    for (ULONG i = 0; i < EMU_ISOCH_SLOTS; i++) {
        PEMU_ISOCH_SLOT slot = &S->Slots[i];

        RtlZeroMemory(slot, sizeof(*slot));
        KeInitializeEvent(&slot->Done, NotificationEvent, FALSE);
        status = USBD_IsochUrbAllocate(Ctx->UsbdHandle, 1, &slot->Urb);
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

    // Reserve the frame window: current frame plus the margin, slot i covering
    // the i-th period out. Late submissions are counted, not hidden. This URB
    // must come from the contract-602 handle too: the 2.0.0.24 stack-local
    // frameUrb caused bugcheck 0x144/3 on the first hardware START.
    status = USBD_UrbAllocate(Ctx->UsbdHandle, &frameUrb);
    if (NT_SUCCESS(status)) {
        frameUrb->UrbGetCurrentFrameNumber.Hdr.Function =
            URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
        frameUrb->UrbGetCurrentFrameNumber.Hdr.Length =
            (USHORT)sizeof(struct _URB_GET_CURRENT_FRAME_NUMBER);
        frameUrb->UrbGetCurrentFrameNumber.Hdr.Status = 0;
        status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, frameUrb);
    }
    if (NT_SUCCESS(status)) {
        // Calibrate the counter units: query twice across 10 ms of interrupt
        // time. Millisecond frames give delta 10; a microframe-based counter
        // gives 80. The pacing below scales with it - wrong units schedule
        // every packet in the past, the controller completes them all at
        // once, and the wire runs many times faster than real audio.
        ULONG f1 = frameUrb->UrbGetCurrentFrameNumber.FrameNumber;
        LARGE_INTEGER calDelay;

        calDelay.QuadPart = -10 * 10000LL;   // 10 ms
        KeDelayExecutionThread(KernelMode, FALSE, &calDelay);
        frameUrb->UrbGetCurrentFrameNumber.Hdr.Status = 0;
        status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, frameUrb);
        if (NT_SUCCESS(status)) {
            ULONG f2 = frameUrb->UrbGetCurrentFrameNumber.FrameNumber;
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
        if (frameUrb != NULL) {
            USBD_UrbFree(Ctx->UsbdHandle, frameUrb);
        }
        EmuIsochFreeStream(Ctx, Direction);
        return status;
    }
    base = frameUrb->UrbGetCurrentFrameNumber.FrameNumber +
           EMU_ISOCH_FRAME_MARGIN;
    S->PaceOrigin = KeQueryInterruptTime();
    S->PaceBaseFrame = frameUrb->UrbGetCurrentFrameNumber.FrameNumber;
    USBD_UrbFree(Ctx->UsbdHandle, frameUrb);
    for (ULONG i = 0; i < EMU_ISOCH_SLOTS; i++) {
        S->Slots[i].NextFrame = base + i * S->PeriodMs;
    }
    // The wire verdict: the first StartFrame, for correlating a bus trace.
    EmuNoteStatusValue(Ctx, Direction == EMU_STREAM_RENDER
                            ? L"TestFrame0" : L"TestFrame1", base);

    // The feedback reader, allocated before the worker starts: without an
    // actively serviced feedback endpoint the asynchronous OUT sink never
    // locks its DAC (the 2.0.0.23 - 2.0.0.58 silence). The reader runs on
    // its own thread with a synchronous one-packet-at-a-time submit through
    // EmuUsbSubmitUrb - the shared-IRP asynchronous variant of 2.0.0.61/62
    // crashed inside the controller's completion APC machinery.

    if (S->FeedbackPipe != NULL) {
        S->FbBuffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, 8,
                                              EMU_POOLTAG);
        if (S->FbBuffer == NULL) {
            EmuIsochFreeStream(Ctx, Direction);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(S->FbBuffer, 8);
        status = USBD_IsochUrbAllocate(Ctx->UsbdHandle, 1, &S->FbUrb);
        if (!NT_SUCCESS(status)) {
            EmuIsochFreeStream(Ctx, Direction);
            return status;
        }
    }

    S->Active = TRUE;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&threadHandle, THREAD_ALL_ACCESS, &oa, NULL,
                                  NULL, EmuIsochWorker, S);
    if (!NT_SUCCESS(status)) {
        S->Active = FALSE;
        EmuIsochFreeStream(Ctx, Direction);
        return status;
    }
    // Keep the kernel handle until the thread object reference exists. If that
    // rare reference operation fails, StopStream can still abort and wait on
    // the handle, and FreeStream closes it exactly once.
    S->ThreadHandle = threadHandle;
    status = ObReferenceObjectByHandle(threadHandle, THREAD_ALL_ACCESS,
                                       *PsThreadType, KernelMode,
                                       &S->ThreadObject, NULL);
    if (!NT_SUCCESS(status)) {
        S->ThreadObject = NULL;
        EmuIsochStopStream(Ctx, Direction);
        return status;
    }
    S->ThreadHandle = NULL;
    ZwClose(threadHandle);

    // The feedback thread: started last, aborted first (StopStream aborts
    // both pipes before waiting on the threads).
    S->FbThreadHandle = NULL;
    S->FbThreadObject = NULL;
    if (S->FbUrb != NULL) {
        status = PsCreateSystemThread(&threadHandle, THREAD_ALL_ACCESS, &oa,
                                      NULL, NULL, EmuIsochFeedbackWorker, S);
        if (!NT_SUCCESS(status)) {
            S->FbThreadHandle = NULL;
            EmuIsochStopStream(Ctx, Direction);
            return status;
        }
        S->FbThreadHandle = threadHandle;
        if (NT_SUCCESS(ObReferenceObjectByHandle(threadHandle,
                                                 THREAD_ALL_ACCESS,
                                                 *PsThreadType, KernelMode,
                                                 &S->FbThreadObject, NULL))) {
            ZwClose(threadHandle);
            S->FbThreadHandle = NULL;
        } else {
            S->FbThreadObject = NULL;
        }
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
    _In_ PEMU_ISOCH_RENDER_COMPLETED  RenderCompleted
    )
{
    const EMU_STREAM_ALT* alt;
    PEMU_ISOCH_STREAM S = &Ctx->Isoch[EMU_STREAM_RENDER];
    NTSTATUS status;

    PAGED_CODE();

    if (RateHz != 48000 || CallbackContext == NULL || FillRender == NULL ||
        RenderCompleted == NULL) {
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
    // Probe instrumentation: the assignment happened - the absence of this
    // value with a successful start means the run never reached this line.
    EmuNoteStatusValue(Ctx, L"FillSet", 0x10000001);
    S->RenderCompleted = RenderCompleted;
    S->BaseFrames = BaseFrames;
    status = EmuIsochStartStream(Ctx, EMU_STREAM_RENDER, alt, RateHz);
    if (!NT_SUCCESS(status)) {
        S->CallbackContext = NULL;
        S->FillRender = NULL;
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
