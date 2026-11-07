// EmuUSBAudio driver entry.
// SPDX-License-Identifier: LGPL-2.1-or-later
// initguid first: the contract header's DEFINE_GUID becomes the one
// definition in this translation unit, because IoCreateDeviceSecure below
// takes the control-device class GUID by address. Everywhere else the same
// DEFINE_GUID stays an extern declaration.
#include <initguid.h>
#include "emu.h"
#include <wdmsec.h>   // IoCreateDeviceSecure (wdmsec.lib)

// Verbose enough to follow a PnP soak run out of the box; Parameters\TraceLevel
// in the service key overrides it.
ULONG EmuTraceLevel = EMU_LEVEL_TERSE;

// PortCls installs its own unload routine during PcInitializeAdapterDriver; we
// chain it rather than replace it.
static PDRIVER_UNLOAD g_PortClsUnload = NULL;

// The stage-4c control device: driver-global, created in DriverEntry, deleted
// in DriverUnload. The panel opens it as \\.\\EmuUSBAudioCtl.
static PDEVICE_OBJECT g_ControlDevice = NULL;
// The service key path, copied out of DriverEntry: the PnP manager's buffer is
// only guaranteed while DriverEntry runs, and the diagnostics breadcrumb
// writes into the Parameters subkey long after that.
static WCHAR g_RegistryPathBuffer[512];
UNICODE_STRING EmuDriverRegistryPath;

#pragma code_seg("PAGE")
static VOID EmuReadTraceLevel(_In_opt_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS            status;
    HANDLE              keyHandle = NULL;
    OBJECT_ATTRIBUTES   objectAttributes;
    UNICODE_STRING      path;
    WCHAR               pathBuffer[512];
    UNICODE_STRING      valueName;
    WCHAR               valueNameBuffer[] = L"TraceLevel";
    PKEY_VALUE_PARTIAL_INFORMATION valueInformation;
    ULONG               bufferLength;
    ULONG               resultLength = 0;

    PAGED_CODE();

    if (RegistryPath == NULL || RegistryPath->Buffer == NULL ||
        RegistryPath->Length == 0) {
        return;
    }

    // ...\Services\EmuUSBAudio\Parameters
    bufferLength = RegistryPath->Length + sizeof(L"\\Parameters");
    if (bufferLength > sizeof(pathBuffer)) {
        return;
    }
    path.Buffer = pathBuffer;
    path.Length = 0;
    path.MaximumLength = (USHORT)sizeof(pathBuffer);
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&path, RegistryPath)) ||
        !NT_SUCCESS(RtlAppendUnicodeToString(&path, L"\\Parameters"))) {
        return;
    }

    InitializeObjectAttributes(&objectAttributes,
                               &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    status = ZwOpenKey(&keyHandle, KEY_READ, &objectAttributes);
    if (!NT_SUCCESS(status)) {
        return;
    }

    RtlInitUnicodeString(&valueName, valueNameBuffer);

    bufferLength = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG);
    valueInformation = (PKEY_VALUE_PARTIAL_INFORMATION)
        ExAllocatePool2(POOL_FLAG_PAGED, bufferLength, EMU_POOLTAG);
    if (valueInformation != NULL) {
        status = ZwQueryValueKey(keyHandle,
                                 &valueName,
                                 KeyValuePartialInformation,
                                 valueInformation,
                                 bufferLength,
                                 &resultLength);
        if (NT_SUCCESS(status) &&
            valueInformation->Type == REG_DWORD &&
            valueInformation->DataLength == sizeof(ULONG)) {
            ULONG level = *(PULONG)valueInformation->Data;
            if (level >= EMU_LEVEL_ERROR && level <= EMU_LEVEL_BLAB) {
                EmuTraceLevel = level;
            }
        }
        ExFreePoolWithTag(valueInformation, EMU_POOLTAG);
    }

    ZwClose(keyHandle);
}

#pragma code_seg("PAGE")
extern "C" VOID DriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    EMU_LOG_TERSE(STR_MODULENAME "unloaded\n");

    // The control device lives exactly as long as the driver: created in
    // DriverEntry, deleted here, before PortCls unloads the code its own
    // dispatch needs.
    if (g_ControlDevice != NULL) {
        UNICODE_STRING dosName;
        RtlInitUnicodeString(&dosName, L"\\??\\EmuUSBAudioCtl");
        IoDeleteSymbolicLink(&dosName);
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = NULL;
    }

    if (g_PortClsUnload != NULL) {
        g_PortClsUnload(DriverObject);
    }
}

// INIT: the OS discards this section once the driver is loaded, so DriverEntry
// must live here (and may call into PAGE, it runs at PASSIVE_LEVEL).
#pragma code_seg("INIT")
extern "C" NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;

    // The service key path is needed later by the probe breadcrumb; the
    // PnP manager's buffer is only guaranteed while DriverEntry runs.
    if (RegistryPath != NULL && RegistryPath->Buffer != NULL &&
        RegistryPath->Length / sizeof(WCHAR) <
        (sizeof(g_RegistryPathBuffer) / sizeof(WCHAR)) - 8) {
        RtlCopyMemory(g_RegistryPathBuffer, RegistryPath->Buffer,
                      RegistryPath->Length);
        g_RegistryPathBuffer[RegistryPath->Length / sizeof(WCHAR)] = 0;
        EmuDriverRegistryPath.Length = RegistryPath->Length;
        EmuDriverRegistryPath.MaximumLength =
            (USHORT)sizeof(g_RegistryPathBuffer);
        EmuDriverRegistryPath.Buffer = g_RegistryPathBuffer;
        EmuNoteStatusValue(NULL, L"EntryBread", 0x00000028);
    }
    EmuReadTraceLevel(RegistryPath);

    status = PcInitializeAdapterDriver(DriverObject,
                                       RegistryPath,
                                       (PDRIVER_ADD_DEVICE)AddDevice);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "PcInitializeAdapterDriver failed, 0x%x\n",
                      status);
        return status;
    }

    // Wrap PnP so stop/remove/surprise-removal can be paired with our own
    // teardown, and device-control so the two private panel codes are answered
    // here while everything else still reaches PortCls. Every other minor code
    // and major function goes to PortCls untouched.
    DriverObject->MajorFunction[IRP_MJ_PNP] = EmuPnpHandler;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = EmuDeviceControlHandler;
    // CREATE/CLOSE/CLEANUP: the control device completes them itself, the
    // FDO forwards to PortCls (see emu_disp.cpp).
    DriverObject->MajorFunction[IRP_MJ_CREATE] = EmuCreateCloseHandler;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = EmuCreateCloseHandler;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = EmuCreateCloseHandler;

    // DriverEntry runs before any device exists, so an unload routine is
    // allowed here. PortCls has installed its own; keep it reachable.
    g_PortClsUnload = DriverObject->DriverUnload;
    DriverObject->DriverUnload = DriverUnload;

    // Stage 4c: the control device the panel opens. KS/PortCls rejects
    // IRP_MJ_CREATE on the adapter FDO (no create item for an empty file
    // name; hardware run 2026-11-04), so the PDO-registered interface could
    // never be opened - this named device object is the panel way in
    // instead. Admin-only, one per driver; the multi-instance name story is
    // M9's. This private control ACL is deliberately stricter than the audio
    // FDO ACL in the INF, which must also admit the Windows Audio service.
    UNICODE_STRING ctlName, ctlDosName, ctlSddl;
    RtlInitUnicodeString(&ctlName, L"\\Device\\EmuUSBAudioCtl");
    RtlInitUnicodeString(&ctlDosName, L"\\??\\EmuUSBAudioCtl");
    RtlInitUnicodeString(&ctlSddl,
                         L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    status = IoCreateDeviceSecure(DriverObject, sizeof(ULONG), &ctlName,
                                  FILE_DEVICE_UNKNOWN, 0, FALSE, &ctlSddl,
                                  &GUID_DEVINTERFACE_EMUUSBAUDIO,
                                  &g_ControlDevice);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "control device creation failed, 0x%x\n",
                      status);
        return status;
    }
    *(PULONG)g_ControlDevice->DeviceExtension = EMU_CTL_SIGNATURE;
    g_ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    status = IoCreateSymbolicLink(&ctlDosName, &ctlName);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "control device symlink failed, 0x%x\n",
                      status);
        IoDeleteDevice(g_ControlDevice);
        g_ControlDevice = NULL;
        return status;
    }

    EMU_LOG_TERSE(STR_MODULENAME "loaded, M2 control plane, trace %lu\n",
                  EmuTraceLevel);

    return STATUS_SUCCESS;
}
