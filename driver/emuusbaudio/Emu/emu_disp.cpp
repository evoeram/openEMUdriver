// EmuUSBAudio — device-control and create/close dispatch (M2, stages 4b-4c).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// This driver owns two kinds of device objects, and the shared DriverObject
// dispatch table serves both, distinguished by device type plus the control
// signature:
//
//   * the PortCLS adapter FDO (FILE_DEVICE_SOUND): KS/wdmaud drive the audio
//     stack through it, and everything this driver does not recognize goes to
//     PcDispatchIrp untouched. The two private panel codes were answered here
//     through stage 4b, until the hardware run proved the deeper problem:
//     IRP_MJ_CREATE on the FDO is answered by KS, which has no create item for
//     an empty file name, so a panel handle on the FDO can never be opened.
//
//   * the control device (FILE_DEVICE_UNKNOWN + EMU_CTL_SIGNATURE, created in
//     DriverEntry as \\.\EmuUSBAudioCtl): the panel's way in since stage 4c.
//     CREATE/CLOSE complete here, the two private codes are served from the
//     started adapter instance, and a foreign code gets
//     STATUS_INVALID_DEVICE_REQUEST — there is no KS behind this object.
//
// The rules themselves — what counts as ours, what a caller with wrong buffers
// gets — are EmuIoctlClassify/EmuIoctlIsDeviceUsable in Emu/emu_ioctl.cpp,
// covered by driver/utest.sh ioctl. What is left here is the IRP plumbing.
//
// The handler stays in the default (nonpaged) section on purpose: it sits in
// front of PcDispatchIrp for every device-control IRP the stack sends, and KS
// property traffic does not guarantee PASSIVE_LEVEL. The fill path (the two
// private codes) needs PASSIVE..APC for the adapter's state lock and checks
// that on its own.
#include "emu.h"

// The adapter context the control device serves. Set at the end of a
// successful start, cleared on stop and remove; read atomically.
PEMU_DEVICE_CONTEXT EmuControlAdapter = NULL;

// Complete one of the two private queries against an adapter context. The
// reject case completes with the classify verdict; the fill case takes the
// adapter's state lock, because Caps can be freed under it by a STOP running
// in parallel (an open panel handle survives a disable/enable) and the
// settings cache is rewritten on the same lock.
static NTSTATUS EmuCompleteQuery(
    _Inout_ PIRP                      Irp,
    _Inout_ PEMU_DEVICE_CONTEXT       Ctx,
    _In_ const EMU_IOCTL_DECISION*    Decision
    )
{
    PVOID reply = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status;

    if (Decision->Action == EmuIoctlReject) {
        Irp->IoStatus.Status = Decision->Status;
        Irp->IoStatus.Information = 0;
        status = Decision->Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    // METHOD_BUFFERED user requests always arrive with the buffer the I/O
    // manager allocated; a kernel caller that builds its own IRP can hand
    // over NULL, and classify only saw the lengths.
    if (reply == NULL) {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
    } else if (Decision->Action == EmuIoctlGetCaps) {
        ExAcquireFastMutex(&Ctx->StateLock);
        // Non-NULL here means no release is mid-flight; the usable answer
        // classify gave is re-checked where the race actually is.
        if (Ctx->Caps == NULL) {
            Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
            Irp->IoStatus.Information = 0;
        } else {
            EmuIoctlFillCaps((PEMU_CAPS_INFO)reply, Ctx->Caps,
                             EMU_OP_IMPLEMENTED);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = Decision->OutputBytes;
        }
        ExReleaseFastMutex(&Ctx->StateLock);
    } else {
        // GET_STATE and all four SET operations answer with the state
        // snapshot. The SETs read their EMU_SET_INPUT from this same buffer
        // first - METHOD_BUFFERED hands us the input stage right here, and
        // the reply overwrites it - then run the request through
        // EmuUsbExecuteSet, which serializes against other SETs and stops
        // under the state lock and records the outcome for LastOperation-
        // Status. A refused or failed SET still completes STATUS_SUCCESS:
        // the IOCTL worked, and the verdict travels in the state reply
        // (LastOperationStatus plus whatever ValidMask shows), which is what
        // that contract field exists for.
        EMU_IOCTL_ACTION action = Decision->Action;
        ULONG value = 0;

        if (action != EmuIoctlGetState) {
            PEMU_SET_INPUT input = (PEMU_SET_INPUT)reply;

            if (input->Size != sizeof(EMU_SET_INPUT) ||
                input->Version != EMU_IOCTL_VERSION) {
                // A caller speaking a different dialect of the contract is
                // refused before anything reaches the device.
                Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
                Irp->IoStatus.Information = 0;
                status = STATUS_INVALID_PARAMETER;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return status;
            }
            value = input->Value;
            status = EmuUsbExecuteSet(Ctx, action, value);
        } else {
            status = STATUS_SUCCESS;
        }

        ULONG state = (ULONG)InterlockedCompareExchange(&Ctx->State, 0, 0);
        ExAcquireFastMutex(&Ctx->StateLock);
        EmuIoctlFillState((PEMU_STATE_INFO)reply, &Ctx->Settings,
                          state,
                          Ctx->StartCount,
                          Ctx->StopCount,
                          Ctx->RemoveCount,
                          Ctx->LastOperationStatus);
        ExReleaseFastMutex(&Ctx->StateLock);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = Decision->OutputBytes;
    }

    status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

// The adapter FDO path (stage 4b): remove lock, classify, forward foreign
// codes to PortCls, answer the private ones.
static NTSTATUS EmuAdapterDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp
    )
{
    PEMU_DEVICE_CONTEXT context = EMU_CONTEXT(DeviceObject);
    PIO_STACK_LOCATION  stack = IoGetCurrentIrpStackLocation(Irp);
    EMU_IOCTL_CALL      call;
    EMU_IOCTL_DECISION  decision;
    LONG                state;
    BOOLEAN             locked = FALSE;
    NTSTATUS            status;

    // Same pattern as EmuPnpHandler: an instance whose start never reached the
    // context initialization has no remove lock. Its extension is zeroed, so
    // the classify below reads a clean "not started" and refuses our codes;
    // everything else still goes to PortCls.
    if (context->Signature == EMU_CTX_SIGNATURE && context->RemoveLockReady) {
        status = IoAcquireRemoveLock(&context->RemoveLock, Irp);
        if (!NT_SUCCESS(status)) {
            // Removal is already in progress; the IRP is dead on arrival.
            Irp->IoStatus.Status = status;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }
        locked = TRUE;
    }

    call.ControlCode        = stack->Parameters.DeviceIoControl.IoControlCode;
    call.InputBufferLength  = (ULONG)stack->Parameters.DeviceIoControl.InputBufferLength;
    call.OutputBufferLength = (ULONG)stack->Parameters.DeviceIoControl.OutputBufferLength;

    // Atomic read of the state the PnP handler exchanges. StartCount and the
    // like are plain diagnostic counters and are read without a lock; the caps
    // and the settings cache, which the replies are built from, are taken
    // under StateLock in the fill path.
    state = InterlockedCompareExchange(&context->State, 0, 0);
    EmuIoctlClassify(&call, EmuIoctlIsDeviceUsable((ULONG)state, context->Caps),
                     &decision);

    if (decision.Action == EmuIoctlForward) {
        // PortCls owns this FDO: KS properties, wdmaud and every unknown code
        // must reach it untouched. A "helpful" refusal here would be a bug
        // report about Windows audio, not about the panel.
        status = PcDispatchIrp(DeviceObject, Irp);
        if (locked) {
            IoReleaseRemoveLock(&context->RemoveLock, Irp);
        }
        return status;
    }

    // Our own codes. The fill path takes the state lock, which requires
    // PASSIVE..APC; nothing kernel-side sends these codes — but a future
    // kernel caller at DISPATCH gets an outright refusal instead of a mutex
    // bugcheck under Verifier.
    if (KeGetCurrentIrql() > APC_LEVEL) {
        decision.Action = EmuIoctlReject;
        decision.Status = STATUS_INVALID_DEVICE_REQUEST;
    }

    status = EmuCompleteQuery(Irp, context, &decision);

    if (locked) {
        IoReleaseRemoveLock(&context->RemoveLock, Irp);
    }
    return status;
}

// The control device path (stage 4c): no PortCls, no PnP of its own; the
// reply comes from whichever adapter instance is started right now.
static NTSTATUS EmuControlDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp
    )
{
    PIO_STACK_LOCATION  stack = IoGetCurrentIrpStackLocation(Irp);
    PEMU_DEVICE_CONTEXT context;
    EMU_IOCTL_CALL      call;
    EMU_IOCTL_DECISION  decision;
    NTSTATUS            status;

    UNREFERENCED_PARAMETER(DeviceObject);

    call.ControlCode        = stack->Parameters.DeviceIoControl.IoControlCode;
    call.InputBufferLength  = (ULONG)stack->Parameters.DeviceIoControl.InputBufferLength;
    call.OutputBufferLength = (ULONG)stack->Parameters.DeviceIoControl.OutputBufferLength;

    context = (PEMU_DEVICE_CONTEXT)
        InterlockedCompareExchangePointer((PVOID volatile*)&EmuControlAdapter,
                                          NULL, NULL);
    if (context != NULL) {
        ULONG state = (ULONG)InterlockedCompareExchange(&context->State, 0, 0);
        EmuIoctlClassify(&call,
                         EmuIoctlIsDeviceUsable(state, context->Caps), &decision);
    } else {
        EmuIoctlClassify(&call, FALSE, &decision);
    }

    if (decision.Action == EmuIoctlForward) {
        // There is no KS behind this device object; a code the contract does
        // not define is a client bug, not traffic for somebody else.
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (context == NULL) {
        // The usable check already refused, but classify runs before the
        // pointer is re-read under the lock below — never dereference a
        // context that may have gone away in between.
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_READY;
    }

    if (KeGetCurrentIrql() > APC_LEVEL) {
        decision.Action = EmuIoctlReject;
        decision.Status = STATUS_INVALID_DEVICE_REQUEST;
    }

    // The panel handle lives on the control device, so the FDO's remove lock
    // is not held by anything yet. Hold the FDO object alive across the fill
    // (REMOVE deletes it with our reference outstanding), then take its remove
    // lock: after IoReleaseRemoveLockAndWait has begun, every acquire fails,
    // and before it, the wait waits for us.
    ObReferenceObject(context->Fdo);
    status = IoAcquireRemoveLock(&context->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(context->Fdo);
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_READY;
    }

    status = EmuCompleteQuery(Irp, context, &decision);

    IoReleaseRemoveLock(&context->RemoveLock, Irp);
    ObDereferenceObject(context->Fdo);
    return status;
}

extern "C" NTSTATUS NTAPI EmuDeviceControlHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp
    )
{
    if (DeviceObject->DeviceType == FILE_DEVICE_UNKNOWN &&
        DeviceObject->DeviceExtension != NULL &&
        *(PULONG)DeviceObject->DeviceExtension == EMU_CTL_SIGNATURE) {
        return EmuControlDeviceControl(DeviceObject, Irp);
    }
    return EmuAdapterDeviceControl(DeviceObject, Irp);
}

// CREATE/CLOSE/CLEANUP: the control device is ours end to end, the adapter
// FDO is PortCls's. The hardware run (2026-11-04) showed KS answering CREATE
// on the FDO with OBJECT_NAME_NOT_FOUND — the reason the panel cannot open
// the FDO and this control device exists.
extern "C" NTSTATUS NTAPI EmuCreateCloseHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp
    )
{
    if (DeviceObject->DeviceType == FILE_DEVICE_UNKNOWN &&
        DeviceObject->DeviceExtension != NULL &&
        *(PULONG)DeviceObject->DeviceExtension == EMU_CTL_SIGNATURE) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
    // Adapter FDO: KS filter handles (M4 and beyond) must reach PortCls.
    return PcDispatchIrp(DeviceObject, Irp);
}
