// PnP lifecycle for the EmuUSBAudio PortCls FDO (M1: no audio, no USB I/O).
// SPDX-License-Identifier: LGPL-2.1-or-later
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
        context->RemoveLockReady = TRUE;
        context->Signature = EMU_CTX_SIGNATURE;
    }

    // A device can be stopped and started again without being removed, so this
    // runs more than once per instance. The remove lock and the counters
    // survive; only the state changes.
    context->StartCount++;
    InterlockedExchange(&context->State, (LONG)EmuStateStarted);

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
        EMU_LOG_TERSE(STR_MODULENAME "stopped (%p), stop #%lu\n",
                      DeviceObject, context->StopCount);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        InterlockedExchange(&context->State, (LONG)EmuStateSurpriseRemoved);
        EMU_LOG_TERSE(STR_MODULENAME "surprise removal (%p)\n", DeviceObject);
        // No resources are released here: the extension lives until REMOVE,
        // which always follows.
        break;

    case IRP_MN_REMOVE_DEVICE:
        InterlockedExchange(&context->State, (LONG)EmuStateRemoved);
        context->RemoveCount++;
        if (locked) {
            // Waits for every other holder before we let the extension go:
            // PcDispatchIrp deletes the FDO, and the FDO owns our context.
            IoReleaseRemoveLockAndWait(&context->RemoveLock, Irp);
            context->RemoveLockReady = FALSE;
            locked = FALSE;
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
