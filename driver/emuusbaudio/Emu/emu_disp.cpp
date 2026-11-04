// EmuUSBAudio — IRP_MJ_DEVICE_CONTROL dispatch (M2, stage 4b).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The FDO serves two kinds of device-control traffic and the split decides
// everything: KS/wdmaud drive the audio stack through it (their own device
// type, FILE_DEVICE_KS), and from this build the panel drives the E-MU controls
// through the two private codes of Emu/emu_ioctl.h (FILE_DEVICE_UNKNOWN). The
// rules themselves — what counts as ours, what a caller with wrong buffers
// gets — are EmuIoctlClassify/EmuIoctlIsDeviceUsable in Emu/emu_ioctl.cpp and
// are covered by driver/utest.sh ioctl. What is left here is the IRP plumbing
// those rules cannot know about: the remove lock, the buffered SystemBuffer,
// and forwarding everything else to PortCls exactly as it went before this
// handler existed.
//
// The handler stays in the default (nonpaged) section on purpose. It sits in
// front of PcDispatchIrp for every device-control IRP the stack sends, and KS
// property traffic does not guarantee PASSIVE_LEVEL, so the PnP-file pattern of
// a "PAGE" section would be wrong here. The fill path (our two codes) needs
// PASSIVE..APC for the state lock and checks that on its own.
#include "emu.h"

extern "C" NTSTATUS NTAPI EmuDeviceControlHandler(
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
    // under StateLock below.
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
    // PASSIVE..APC; nothing kernel-side sends these codes — the device
    // interface is the panel's — but a future kernel caller at DISPATCH gets an
    // outright refusal instead of a mutex bugcheck under Verifier.
    if (KeGetCurrentIrql() > APC_LEVEL) {
        decision.Action = EmuIoctlReject;
        decision.Status = STATUS_INVALID_DEVICE_REQUEST;
    }

    if (decision.Action == EmuIoctlReject) {
        Irp->IoStatus.Status = decision.Status;
        Irp->IoStatus.Information = 0;
        status = decision.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    } else {
        PVOID reply = Irp->AssociatedIrp.SystemBuffer;

        // METHOD_BUFFERED user requests always arrive with the buffer the I/O
        // manager allocated; a kernel caller that builds its own IRP can hand
        // over NULL, and classify only saw the lengths.
        if (reply == NULL) {
            Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
            Irp->IoStatus.Information = 0;
        } else if (decision.Action == EmuIoctlGetCaps) {
            ExAcquireFastMutex(&context->StateLock);
            // Caps can be freed under this lock by a STOP running in parallel —
            // an old panel handle can still be open across a disable/enable —
            // so the usable answer classify gave is re-checked where the race
            // actually is. Non-NULL here means no release is mid-flight.
            if (context->Caps == NULL) {
                Irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
                Irp->IoStatus.Information = 0;
            } else {
                EmuIoctlFillCaps((PEMU_CAPS_INFO)reply, context->Caps,
                                 EMU_OP_IMPLEMENTED);
                Irp->IoStatus.Status = STATUS_SUCCESS;
                Irp->IoStatus.Information = decision.OutputBytes;
            }
            ExReleaseFastMutex(&context->StateLock);
        } else {
            // GET_STATE reads only the cache and the counters; LastOperation
            // is 0 because no SET exists in this build's contract yet.
            ExAcquireFastMutex(&context->StateLock);
            EmuIoctlFillState((PEMU_STATE_INFO)reply, &context->Settings,
                              (ULONG)state,
                              context->StartCount,
                              context->StopCount,
                              context->RemoveCount,
                              (NTSTATUS)0);
            ExReleaseFastMutex(&context->StateLock);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = decision.OutputBytes;
        }

        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    if (locked) {
        IoReleaseRemoveLock(&context->RemoveLock, Irp);
    }
    return status;
}
