// PnP lifecycle for the EmuUSBAudio PortCls FDO. Start brings up the USB client
// (emu_usb.cpp) and the private control interface (stage 4b); stop and remove
// take both down again. No audio paths yet.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
#include "emu.h"

#pragma code_seg("PAGE")
extern "C" NTSTATUS AddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
    )
{
    PAGED_CODE();

    // PortCls creates the FDO, attaches it to the USB PDO and owns both. Its
    // device extension is the PortCls region plus our context appended behind
    // it; PcAddAdapterDevice rejects a size between zero and
    // PORT_CLASS_DEVICE_EXTENSION_SIZE, hence the sum.
    return PcAddAdapterDevice(DriverObject,
                              PhysicalDeviceObject,
                              EmuStartDevice,
                              EMU_MAX_SUBDEVICES,
                              PORT_CLASS_DEVICE_EXTENSION_SIZE +
                                  sizeof(EMU_DEVICE_CONTEXT));
}

#pragma code_seg("PAGE")
extern "C" NTSTATUS EmuStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp,
    _In_ PRESOURCELIST  ResourceList
    )
{
    PEMU_DEVICE_CONTEXT   context = EMU_CONTEXT(DeviceObject);
    PPORT_CLASS_EXTENSION portCls =
        (PPORT_CLASS_EXTENSION)DeviceObject->DeviceExtension;
    PDEVICE_OBJECT        pdo = NULL;
    NTSTATUS              status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(ResourceList);

    // PortCls zeroes the extension, so a fresh instance has no signature. Any
    // other value than zero or ours means the assumed layout is wrong, and
    // writing through it would corrupt PortCls memory.
    if (context->Signature != 0 && context->Signature != EMU_CTX_SIGNATURE) {
        EMU_LOG_ERROR(STR_MODULENAME "unexpected data at the extension tail "
                      "(0x%x): layout assumption is wrong, refusing to start\n",
                      context->Signature);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    status = PcGetPhysicalDeviceObject(DeviceObject, &pdo);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "PcGetPhysicalDeviceObject failed, 0x%x\n",
                      status);
        return status;
    }
    if (pdo != portCls->PhysicalDeviceObject) {
        EMU_LOG_ERROR(STR_MODULENAME "PDO mismatch (%p vs %p): layout assumption "
                      "is wrong, refusing to start\n",
                      pdo, portCls->PhysicalDeviceObject);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (context->Signature == 0) {
        context->Fdo = DeviceObject;
        context->Pdo = pdo;
        IoInitializeRemoveLock(&context->RemoveLock, EMU_POOLTAG, 0, 0);
        ExInitializeFastMutex(&context->StateLock);
        context->RemoveLockReady = TRUE;
        context->Signature = EMU_CTX_SIGNATURE;
    }

    // A device can be stopped and started again without being removed, so this
    // runs more than once per instance. The remove lock, the state lock and the
    // counters survive; only the state changes.
    context->StartCount++;

    // The USB client lives and dies with each start: the handle is registered
    // against the PDO and the caps describe the configuration that is active
    // right now. If the device cannot be talked to, the instance is not started.
    status = EmuUsbInitialize(context);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "USB setup failed, 0x%x; start #%lu aborted\n",
                      status, context->StartCount);
        return status;
    }

    // Stage 4c: publish the started instance for the control device. The panel
    // opens \\.\EmuUSBAudioCtl and serves itself from this pointer; between
    // the publish and an IOCTL the control path additionally references the
    // FDO and takes this context's remove lock, so a stop racing the publish
    // cannot hand the panel a dying context.
    InterlockedExchange(&context->State, (LONG)EmuStateStarted);
    InterlockedExchangePointer((PVOID volatile *)&EmuControlAdapter, context);

    EMU_LOG_TERSE(STR_MODULENAME "started (%p), start #%lu\n",
                  DeviceObject, context->StartCount);

    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
extern "C" NTSTATUS NTAPI EmuPnpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp
    )
{
    PEMU_DEVICE_CONTEXT context = EMU_CONTEXT(DeviceObject);
    PIO_STACK_LOCATION  stack = IoGetCurrentIrpStackLocation(Irp);
    BOOLEAN             locked = FALSE;
    NTSTATUS            status;

    PAGED_CODE();

    // An instance that never started has no remove lock at all: PortCls can
    // still deliver a remove for a device whose start failed.
    if (context->Signature == EMU_CTX_SIGNATURE && context->RemoveLockReady) {
        status = IoAcquireRemoveLock(&context->RemoveLock, Irp);
        if (!NT_SUCCESS(status)) {
            // Removal is already in progress; complete rather than forward.
            Irp->IoStatus.Status = status;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }
        locked = TRUE;
    }

    switch (stack->MinorFunction) {
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        // Nothing to give up yet, so always agree and let PortCls drive.
        break;

    case IRP_MN_STOP_DEVICE:
        context->StopCount++;
        InterlockedExchange(&context->State, (LONG)EmuStateStopped);
        // The published instance goes away first: a panel that asks during the
        // stop gets "device not ready" instead of a half-stopped one. The
        // control device object itself stays — it is driver-global and answers
        // "not ready" until the next start. Everything USB goes here; a later
        // START re-registers the handle and re-reads the descriptor. The PDO is
        // still attached, so the close is ordered correctly.
        InterlockedExchangePointer((PVOID volatile *)&EmuControlAdapter, NULL);
        EmuUsbRelease(context);
        EMU_LOG_TERSE(STR_MODULENAME "stopped (%p), stop #%lu\n",
                      DeviceObject, context->StopCount);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        InterlockedExchange(&context->State, (LONG)EmuStateSurpriseRemoved);
        EMU_LOG_TERSE(STR_MODULENAME "surprise removal (%p)\n", DeviceObject);
        // No resources are released here: the extension lives until REMOVE,
        // which always follows. The published instance stays on purpose — a
        // panel that still holds an open control-device handle gets our
        // "device not ready" refusals rather than silence, and the state's
        // SURPRISE_REMOVED value is what the reply reports.
        break;

    case IRP_MN_REMOVE_DEVICE:
        InterlockedExchange(&context->State, (LONG)EmuStateRemoved);
        context->RemoveCount++;
        // The published instance must go before the extension dies inside
        // PcDispatchIrp — after the forward it is unreachable. The control
        // path's own FDO reference and remove lock cover the race where a
        // panel request is mid-fill right now.
        InterlockedExchangePointer((PVOID volatile *)&EmuControlAdapter, NULL);
        if (locked) {
            // Waits for every other holder before we let the extension go:
            // PcDispatchIrp deletes the FDO, and the FDO owns our context.
            IoReleaseRemoveLockAndWait(&context->RemoveLock, Irp);
            context->RemoveLockReady = FALSE;
            locked = FALSE;
        }
        // A surprise-removed instance never stopped, so this also covers the
        // case where the USB client is still live. Idempotent otherwise. An
        // instance whose start failed before the context was initialized has
        // nothing allocated and no initialized state lock, so its release is
        // skipped rather than run against uninitialized sync state.
        if (context->Signature == EMU_CTX_SIGNATURE) {
            EmuUsbRelease(context);
        }
        EMU_LOG_TERSE(STR_MODULENAME "removed (%p), remove #%lu\n",
                      DeviceObject, context->RemoveCount);
        status = PcDispatchIrp(DeviceObject, Irp);
        // DeviceObject and everything reachable from it are gone by now.
        return status;

    default:
        break;
    }

    status = PcDispatchIrp(DeviceObject, Irp);

    if (locked) {
        IoReleaseRemoveLock(&context->RemoveLock, Irp);
    }

    return status;
}

#pragma code_seg()
