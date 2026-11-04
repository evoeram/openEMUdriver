// smoke.c - minimal NT driver to validate the kernel build script
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <ntddk.h>

DRIVER_UNLOAD DriverUnload;
VOID DriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrint("oed: smoke driver unloaded\n");
}

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    DriverObject->DriverUnload = DriverUnload;
    DbgPrint("oed: smoke driver loaded\n");
    return STATUS_SUCCESS;
}
