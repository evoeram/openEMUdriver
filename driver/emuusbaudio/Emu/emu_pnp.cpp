// PnP lifecycle for the EmuUSBAudio PortCls FDO. Start brings up the USB client
// (emu_usb.cpp) and the private control interface (stage 4b); stop and remove
// take both down again. No audio paths yet.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// initguid.h first, and only here: it flips every DEFINE_GUID this translation
// unit subsequently sees into a definition, and this is the one TU that needs
// GUID_DEVINTERFACE_EMUUSBAUDIO (IoRegisterDeviceInterface takes its address).
// Everywhere else — including the user-mode contract test and the future panel
// — the same DEFINE_GUID stays an extern declaration.
#include <initguid.h>
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

// The last control-interface state change, mirrored into the service key's
// Parameters values. The enable state decides whether a panel can open the
// device at all, and on a machine without a kernel-debug capture it is the only
// observable trace of this call; a failure to write the breadcrumb is itself
// silently ignored (diagnostics must not break the path it diagnoses).
#pragma code_seg("PAGE")
static VOID EmuNoteInterfaceState(
    _In_ PEMU_DEVICE_CONTEXT Context,
    _In_ BOOLEAN             Enable,
    _In_ NTSTATUS            Status
    )
{
    HANDLE              keyHandle = NULL;
    OBJECT_ATTRIBUTES   objectAttributes;
    UNICODE_STRING      path;
    WCHAR               pathBuffer[512];
    UNICODE_STRING      valueName;
    WCHAR               nameBuffer[] = L"ControlSetState";
    WCHAR               enabledName[] = L"ControlSetEnabled";

    PAGED_CODE();

    if (EmuDriverRegistryPath.Buffer == NULL) {
        return;
    }

    path.Buffer = pathBuffer;
    path.Length = 0;
    path.MaximumLength = (USHORT)sizeof(pathBuffer);
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&path,
                     &EmuDriverRegistryPath)) ||
        !NT_SUCCESS(RtlAppendUnicodeToString(&path, L"\\Parameters"))) {
        return;
    }

    InitializeObjectAttributes(&objectAttributes,
                               &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    if (!NT_SUCCESS(ZwOpenKey(&keyHandle, KEY_SET_VALUE, &objectAttributes))) {
        return;
    }

    RtlInitUnicodeString(&valueName, nameBuffer);
    ZwSetValueKey(keyHandle, &valueName, 0, REG_DWORD, &Status, sizeof(Status));
    RtlInitUnicodeString(&valueName, enabledName);
    ULONG enabledValue = Enable ? 1 : 0;
    ZwSetValueKey(keyHandle, &valueName, 0, REG_DWORD, &enabledValue,
                  sizeof(enabledValue));
    ZwClose(keyHandle);
}

// Enable or disable the private control interface. Both calls are idempotent:
// the enabled flag keeps the second disable after an already-disabled interface
// from touching the registry, and a device that never registered the interface
// (its start failed early) has no name to act on.
#pragma code_seg("PAGE")
static VOID EmuControlInterfaceSet(
    _In_ PEMU_DEVICE_CONTEXT Context,
    _In_ BOOLEAN             Enable
    )
{
    NTSTATUS status;

    PAGED_CODE();

    if (Context->ControlInterfaceName.Buffer == NULL ||
        Enable == Context->ControlInterfaceEnabled) {
        EMU_LOG_TERSE(STR_MODULENAME "control interface set-state %lu skipped "
                      "(name %p, enabled %lu)\n",
                      (ULONG)Enable, (ULONG_PTR)Context->ControlInterfaceName.Buffer,
                      (ULONG)Context->ControlInterfaceEnabled);
        return;
    }
    status = IoSetDeviceInterfaceState(&Context->ControlInterfaceName, Enable);
    // Always logged: the enabled state is the one thing the panel cannot
    // diagnose from outside, and debug-print capture is not guaranteed on a
    // test machine. The outcome is mirrored into the service key so a plain
    // registry read settles what happened.
    EMU_LOG_TERSE(STR_MODULENAME "control interface set-state %lu -> 0x%x\n",
                  (ULONG)Enable, status);
    EmuNoteInterfaceState(Context, Enable, status);
    if (!NT_SUCCESS(status)) {
        // Not a start/stop failure: the registration stands, the device stays
        // up, only the panel cannot open it right now. Verifier and the event
        // log say why.
        EMU_LOG_WARN(STR_MODULENAME "control interface %wZ: state %lu failed, 0x%x\n",
                     &Context->ControlInterfaceName, (ULONG)Enable, status);
        return;
    }
    Context->ControlInterfaceEnabled = Enable;
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

    // Control-plane step 8 of the plan: the private device interface. The
    // registration is PnP registry state — it survives stop/start and is only
    // created once per instance, but it is re-attempted whenever the name is
    // missing so a start that failed before this point retries it. Failure is a
    // start failure: without the interface there is no panel, and the plan's
    // rollback releases the USB client in reverse order.
    if (context->ControlInterfaceName.Buffer == NULL) {
        status = IoRegisterDeviceInterface(context->Pdo,
                                           &GUID_DEVINTERFACE_EMUUSBAUDIO,
                                           NULL,
                                           &context->ControlInterfaceName);
        if (!NT_SUCCESS(status)) {
            EMU_LOG_ERROR(STR_MODULENAME "IoRegisterDeviceInterface failed, 0x%x; "
                          "start #%lu aborted\n", status, context->StartCount);
            EmuUsbRelease(context);
            return status;
        }
        EMU_LOG_TERSE(STR_MODULENAME "control interface %wZ\n",
                      &context->ControlInterfaceName);
    }

    // Started before the interface is enabled: a panel racing the enable reads
    // a working device, never a half-started one.
    InterlockedExchange(&context->State, (LONG)EmuStateStarted);
    EmuControlInterfaceSet(context, TRUE);

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
        // The interface goes away before the USB client does: a panel that
        // opens during the stop gets "no such device" instead of a device that
        // answers "not ready". Everything USB goes here; a later START
        // re-registers the handle and re-reads the descriptor. The PDO is still
        // attached, so the close is ordered correctly.
        EmuControlInterfaceSet(context, FALSE);
        EmuUsbRelease(context);
        EMU_LOG_TERSE(STR_MODULENAME "stopped (%p), stop #%lu\n",
                      DeviceObject, context->StopCount);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        InterlockedExchange(&context->State, (LONG)EmuStateSurpriseRemoved);
        EMU_LOG_TERSE(STR_MODULENAME "surprise removal (%p)\n", DeviceObject);
        // No resources are released here: the extension lives until REMOVE,
        // which always follows. The interface stays enabled on purpose — a
        // panel that still holds an open handle gets our "device not ready"
        // refusals rather than silence, and the PnP manager disables the
        // registration itself when the remove completes.
        break;

    case IRP_MN_REMOVE_DEVICE:
        InterlockedExchange(&context->State, (LONG)EmuStateRemoved);
        context->RemoveCount++;
        // Disabled and unregistered-name before the wait: both touch only the
        // registry and the name buffer, and the buffer must go before the
        // extension dies inside PcDispatchIrp — after the forward it is
        // unreachable.
        EmuControlInterfaceSet(context, FALSE);
        if (context->ControlInterfaceName.Buffer != NULL) {
            RtlFreeUnicodeString(&context->ControlInterfaceName);
        }
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
