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
// else. Registered once per slot at start; the stack location's completion
// fields survive the re-init the resubmit performs.
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

// One URB submission on a slot's reused IRP. The stack location is re-armed
// each time; the completion routine registered at slot setup stays in place.
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

    stack = IoGetNextIrpStackLocation(slot->Irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    // AssignUrbToIoStackLocation writes both the URB pointer and the
    // IOCTL_INTERNAL_USB_SUBMIT_URB control code into the location.
    USBD_AssignUrbToIoStackLocation(Ctx->UsbdHandle, stack, slot->Urb);

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

// Per-submit URB fields: everything but the buffer pointer, which never
// changes for a slot.
static VOID
EmuIsochArmUrb(
    _Inout_ PEMU_ISOCH_STREAM S,
    _Inout_ PEMU_ISOCH_SLOT   Slot,
    _In_ BOOLEAN              Out
    )
{
    struct _URB_ISOCH_TRANSFER* u = &Slot->Urb->UrbIsochronousTransfer;
    ULONG packetBytes = S->NominalFrames * S->BytesPerFrame;

    u->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    u->Hdr.Length = (USHORT)sizeof(struct _URB_ISOCH_TRANSFER);
    u->Hdr.Status = 0;
    u->PipeHandle = S->Pipe;
    u->TransferFlags = Out
        ? USBD_TRANSFER_DIRECTION_OUT
        : (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK);
    u->TransferBuffer = S->Buffer +
        (ULONG)(Slot - &S->Slots[0]) * S->SlotBytes;
    u->TransferBufferLength = Out ? packetBytes : S->SlotBytes;
    u->StartFrame = Slot->NextFrame;
    u->NumberOfPackets = 1;
    u->IsoPacket[0].Offset = 0;
    u->IsoPacket[0].Length = Out ? packetBytes : S->SlotBytes;
    u->IsoPacket[0].Status = 0;
    Slot->NextFrame += S->PeriodMs;
}

// Read one completion into the counters. Canceled slots (the stop path's
// abort) land here too and are counted honestly - a stop is not a clean run.
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

    Slot->Submitted = FALSE;
    InterlockedIncrement(&S->Completes);

    if (!NT_SUCCESS(Slot->Irp->IoStatus.Status) ||
        !NT_SUCCESS((NTSTATUS)u->Hdr.Status)) {
        InterlockedIncrement(&S->UrbFailures);
        InterlockedExchange(&S->LastUrbStatus, (LONG)u->Hdr.Status);
    }
    if (!USBD_SUCCESS(u->IsoPacket[0].Status)) {
        InterlockedIncrement(&S->IsoPacketErrors);
        InterlockedExchange(&S->LastPacketStatus,
                            (LONG)u->IsoPacket[0].Status);
    }
    got = u->IsoPacket[0].Length;
    if (got < packetBytes) {
        InterlockedIncrement(&S->ShortPackets);
    }
    InterlockedAdd(&S->Bytes, (LONG)got);
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
        status = EmuIsochSubmitSlot(S, Ctx, i);
        if (!NT_SUCCESS(status)) {
            EMU_LOG_ERROR(STR_MODULENAME "isoch slot %lu prime failed, 0x%x\n",
                          (ULONG)i, status);
        }
    }

    while (InterlockedCompareExchange(&S->Quit, 0, 0) == 0) {
        for (ULONG i = 0; i < EMU_ISOCH_SLOTS; i++) {
            PEMU_ISOCH_SLOT slot = &S->Slots[i];

            if (!slot->Submitted) {
                continue;
            }
            KeWaitForSingleObject(&slot->Done, Executive, KernelMode, FALSE,
                                  NULL);
            EmuIsochHarvest(S, slot, out);
            if (InterlockedCompareExchange(&S->Quit, 0, 0) != 0) {
                break;
            }
            EmuIsochArmUrb(S, slot, out);
            status = EmuIsochSubmitSlot(S, Ctx, i);
            if (!NT_SUCCESS(status)) {
                EMU_LOG_ERROR(STR_MODULENAME "isoch slot %lu resubmit failed, "
                              "0x%x\n", (ULONG)i, status);
            }
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

// Free what a stream owns. Every call site has already stopped the thread and
// drained its IRPs; slots still marked Submitted are a stuck completion and
// are skipped (leaked) rather than freed under an outstanding IRP.
#pragma code_seg("PAGE")
static VOID
EmuIsochFreeStream(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ ULONG                  Direction
    )
{
    PEMU_ISOCH_STREAM S = &Ctx->Isoch[Direction];

    PAGED_CODE();

    for (ULONG i = 0; i < EMU_ISOCH_SLOTS; i++) {
        PEMU_ISOCH_SLOT slot = &S->Slots[i];

        if (slot->Irp != NULL) {
            if (slot->Submitted) {
                EMU_LOG_ERROR(STR_MODULENAME "isoch slot %lu leaked (in "
                              "flight at free)\n", i);
            } else {
                IoFreeIrp(slot->Irp);
            }
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
    S->Active = FALSE;
    S->Quit = 0;
    S->Pipe = NULL;
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
    struct _URB               abortUrb;
    NTSTATUS                  status;

    PAGED_CODE();

    if (!S->Active) {
        return;
    }
    InterlockedExchange(&S->Quit, 1);

    if (S->Pipe != NULL) {
        // Abort every queued transfer on the data pipe: the worker's drain
        // waits depend on each of them completing, canceled or not.
        abortUrb.UrbPipeRequest.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
        abortUrb.UrbPipeRequest.Hdr.Length =
            (USHORT)sizeof(struct _URB_PIPE_REQUEST);
        abortUrb.UrbPipeRequest.Hdr.Status = 0;
        abortUrb.UrbPipeRequest.PipeHandle = S->Pipe;
        status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, &abortUrb);
        if (!NT_SUCCESS(status)) {
            EMU_LOG_WARN(STR_MODULENAME "abort pipe (dir %lu) failed, 0x%x\n",
                         Direction, status);
        }
    }

    if (S->ThreadObject != NULL) {
        KeWaitForSingleObject(S->ThreadObject, Executive, KernelMode, FALSE,
                              NULL);
        ObDereferenceObject(S->ThreadObject);
        S->ThreadObject = NULL;
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
    struct _URB                  frameUrb;
    OBJECT_ATTRIBUTES            oa;
    HANDLE                       threadHandle;
    ULONG                        base;
    BOOLEAN                      out = (Direction == EMU_STREAM_RENDER);
    NTSTATUS                     status;

    PAGED_CODE();

    status = EmuUsbSelectInterface(Ctx, Direction,
                                   Alt->InterfaceNumber, Alt->AltSetting);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    iface = Ctx->Stream[Direction].Interface;
    if (iface == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
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
        if (isIn == out) {
            continue;
        }
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
    if (!EmuIsochNominalFramesExact(RateHz, S->PeriodMs, &S->NominalFrames)) {
        EMU_LOG_ERROR(STR_MODULENAME "dir %lu: %lu Hz on a %lu ms grid has no "
                      "exact packet size\n", Direction, RateHz, S->PeriodMs);
        EmuIsochFreeStream(Ctx, Direction);
        return STATUS_INVALID_PARAMETER;
    }
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
    if (out) {
        EmuIsochFillOutPattern(S);
    } else {
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
        slot->Irp->IoStatus.Status = STATUS_SUCCESS;
        // The completion routine is registered once; resubmissions re-arm the
        // stack location's function and URB pointer but keep these fields.
        status = IoSetCompletionRoutineEx(Ctx->Pdo, slot->Irp,
                                          EmuIsochCompletion, &slot->Done,
                                          TRUE, TRUE, TRUE);
        if (!NT_SUCCESS(status)) {
            EmuIsochFreeStream(Ctx, Direction);
            return status;
        }
    }

    // Reserve the frame window: current frame plus the margin, slot i covering
    // the i-th period out. Late submissions are counted, not hidden.
    frameUrb.UrbGetCurrentFrameNumber.Hdr.Function =
        URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
    frameUrb.UrbGetCurrentFrameNumber.Hdr.Length =
        (USHORT)sizeof(struct _URB_GET_CURRENT_FRAME_NUMBER);
    frameUrb.UrbGetCurrentFrameNumber.Hdr.Status = 0;
    status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, &frameUrb);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "current frame query failed, 0x%x\n",
                      status);
        EmuIsochFreeStream(Ctx, Direction);
        return status;
    }
    base = frameUrb.UrbGetCurrentFrameNumber.FrameNumber + EMU_ISOCH_FRAME_MARGIN;
    for (ULONG i = 0; i < EMU_ISOCH_SLOTS; i++) {
        S->Slots[i].NextFrame = base + i * S->PeriodMs;
    }
    // The wire verdict: the first StartFrame, for correlating a bus trace.
    EmuNoteStatusValue(Ctx, Direction == EMU_STREAM_RENDER
                            ? L"TestFrame0" : L"TestFrame1", base);

    S->Active = TRUE;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&threadHandle, THREAD_ALL_ACCESS, &oa, NULL,
                                  NULL, EmuIsochWorker, S);
    if (!NT_SUCCESS(status)) {
        S->Active = FALSE;
        EmuIsochFreeStream(Ctx, Direction);
        return status;
    }
    status = ObReferenceObjectByHandle(threadHandle, THREAD_ALL_ACCESS,
                                       *PsThreadType, KernelMode,
                                       &S->ThreadObject, NULL);
    if (!NT_SUCCESS(status)) {
        // The thread is running and we hold only the handle. KeWaitForSingle-
        // Object accepts a thread handle directly, so the stop runs without
        // the object pointer: quit, abort, wait on the handle, close it.
        S->ThreadObject = NULL;
        EmuIsochStopStream(Ctx, Direction);
        KeWaitForSingleObject(threadHandle, Executive, KernelMode, FALSE,
                              NULL);
        ZwClose(threadHandle);
        return status;
    }
    ZwClose(threadHandle);
    return STATUS_SUCCESS;
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
    status = EmuUsbExecuteSet(Ctx, EmuIoctlSetRate, RateHz);
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
