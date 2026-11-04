// Contract-602 USB client for the EmuUSBAudio PortCls FDO (M2).
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WDM shape: USBD_CreateHandle on the PDO PortCls attached us to, URBs allocated
// by usbdex, submitted as IRP_MJ_INTERNAL_DEVICE_CONTROL with
// IOCTL_INTERNAL_USB_SUBMIT_URB and waited for synchronously.
//
// What this stage does over that channel: fetch the first configuration
// descriptor and run it through the parser, then read one class control that
// changes nothing (the clock-rate CUR). That gives M3 the caps tables it needs
// for pipe handles, puts the descriptor bytes of a live device into the log,
// which is what docs/descriptors is missing, shows on the same run whether
// endpoint-zero class traffic works at all, and starts the settings cache
// (Emu/emu_state.cpp) from whatever that read returns.
// SelectConfiguration/SelectInterface are the next step and they change bus
// state, so they get written against the hardware trace this stage produces
// rather than guessed at twice.
#include "emu.h"
#include <usbioctl.h>   // IOCTL_INTERNAL_USB_SUBMIT_URB

// The header is fetched first to learn wTotalLength; the ceiling is a guard
// against a device that reports an absurd block, not a protocol limit.
#define EMU_CONFIG_BYTES_MAX 4096

#pragma code_seg("PAGE")
static NTSTATUS NTAPI
EmuUsbUrbCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp,
    _Inout_opt_ PVOID   Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);

    // STATUS_MORE_PROCESSING_REQUIRED keeps the IRP ours: the caller reads
    // IoStatus and the URB result after the wait, then frees both.
    if (Context != NULL && Irp->PendingReturned) {
        KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    }
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
EmuUsbSubmitUrb(
    _In_ PDEVICE_OBJECT Target,
    _In_ USBD_HANDLE    Handle,
    _In_ PURB           Urb
    )
{
    PIO_STACK_LOCATION stack;
    PIRP              irp;
    KEVENT            event;
    NTSTATUS          status;

    PAGED_CODE();

    irp = IoAllocateIrp(Target->StackSize, FALSE);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Nothing of this IRP has been consumed yet, so the location the target
    // driver will read is the "next" one — that is where the internal control
    // code and the URB pointer go.
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    USBD_AssignUrbToIoStackLocation(Handle, stack, Urb);

    // Written after the URB location and before IoCallDriver, exactly as
    // "How to Submit an URB" specifies: the completion must sit in the location
    // the target will consume, not in ours.
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    status = IoSetCompletionRoutineEx(Target, irp, EmuUsbUrbCompletion, &event,
                                      TRUE, TRUE, TRUE);
    if (!NT_SUCCESS(status)) {
        IoFreeIrp(irp);
        return status;
    }

    status = IoCallDriver(Target, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    status = irp->IoStatus.Status;
    IoFreeIrp(irp);
    return status;
}

// One GET_DESCRIPTOR(configuration, index 0) into a caller-provided nonpaged
// buffer of exactly Length bytes.
static NTSTATUS
EmuUsbReadConfigDescriptor(
    _In_ PEMU_DEVICE_CONTEXT Ctx,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG                     Length
    )
{
    PURB    urb = NULL;
    ULONG   transferred;
    NTSTATUS status;

    PAGED_CODE();

    status = USBD_UrbAllocate(Ctx->UsbdHandle, &urb);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "USBD_UrbAllocate failed, 0x%x\n", status);
        return status;
    }

    UsbBuildGetDescriptorRequest(urb,
                                 (USHORT)sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                 USB_CONFIGURATION_DESCRIPTOR_TYPE,
                                 0,      // descriptor index: the first configuration
                                 0,      // language id, unused for configuration
                                 Buffer,
                                 NULL,   // plain buffer, no MDL
                                 Length,
                                 NULL);

    status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, urb);
    transferred = urb->UrbControlDescriptorRequest.TransferBufferLength;
    if (NT_SUCCESS(status) && urb->UrbHeader.Status != USBD_STATUS_SUCCESS) {
        EMU_LOG_ERROR(STR_MODULENAME "GET_DESCRIPTOR(config) urb failed, usb status 0x%x\n",
                      (ULONG)urb->UrbHeader.Status);
        status = STATUS_DEVICE_HARDWARE_ERROR;
    }
    USBD_UrbFree(Ctx->UsbdHandle, urb);

    // A short control read is not a partial success: the parser insists on the
    // whole block, and so do we.
    if (NT_SUCCESS(status) && transferred != Length) {
        EMU_LOG_ERROR(STR_MODULENAME "GET_DESCRIPTOR(config) returned %lu of %lu bytes\n",
                      transferred, Length);
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return status;
}

// One class request on endpoint zero. The setup bytes and the payload encoding
// come from Emu/emu_ctrl.cpp; all this function knows is the direction bit and
// the fact that EP0 has no pipe handle.
//
// Payload is the data stage: what the device writes for a device-to-host
// request, what gets sent for a host-to-device one, and NULL only when the
// request has no data stage. It has to stay valid for the whole call, which is
// synchronous.
#pragma code_seg("PAGE")
NTSTATUS
EmuUsbSendControl(
    _In_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ const EMU_CTRL_REQUEST* Request,
    _Inout_opt_ PUCHAR Payload
    )
{
    PURB    urb = NULL;
    ULONG   transferred;
    NTSTATUS status;

    PAGED_CODE();

    status = USBD_UrbAllocate(Ctx->UsbdHandle, &urb);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "USBD_UrbAllocate failed, 0x%x\n", status);
        return status;
    }

    urb->UrbControlTransfer.Hdr.Length = (USHORT)sizeof(struct _URB_CONTROL_TRANSFER);
    urb->UrbControlTransfer.Hdr.Function = URB_FUNCTION_CONTROL_TRANSFER;
    // Endpoint zero is addressed by the setup packet itself, so there is no
    // pipe handle to fill in: the interface and unit live in wIndex.
    urb->UrbControlTransfer.PipeHandle = NULL;
    RtlCopyMemory(urb->UrbControlTransfer.SetupPacket, Request->Bytes,
                  EMU_CTRL_SETUP_BYTES);
    // A short data stage is accepted at the USB level and checked here, so the
    // log shows how many bytes came back instead of only that the transfer died.
    // USBD_DEFAULT_PIPE_TRANSFER is what routes the URB to endpoint zero when
    // PipeHandle is NULL — without it the stack refuses the request with
    // STATUS_INVALID_PARAMETER before the device ever sees it (hardware run
    // 2026-11-04: the GET_CUR probe died with 0xC000000D on exactly this).
    urb->UrbControlTransfer.TransferFlags =
        (Request->Bytes[0] == EMU_CTRL_REQUEST_TYPE_OUT)
            ? (ULONG)(USBD_TRANSFER_DIRECTION_OUT | USBD_DEFAULT_PIPE_TRANSFER)
            : (ULONG)(USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK |
                      USBD_DEFAULT_PIPE_TRANSFER);
    urb->UrbControlTransfer.TransferBufferLength = Request->PayloadLength;
    urb->UrbControlTransfer.TransferBuffer = Payload;
    urb->UrbControlTransfer.TransferBufferMDL = NULL;

    status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, urb);
    transferred = urb->UrbControlTransfer.TransferBufferLength;
    if (NT_SUCCESS(status) && urb->UrbHeader.Status != USBD_STATUS_SUCCESS) {
        EMU_LOG_WARN(STR_MODULENAME "class request cs 0x%02X unit 0x%02X stalled, "
                     "usb status 0x%x\n",
                     Request->Bytes[3], Request->Bytes[5],
                     (ULONG)urb->UrbHeader.Status);
        status = STATUS_DEVICE_HARDWARE_ERROR;
    }
    USBD_UrbFree(Ctx->UsbdHandle, urb);

    if (NT_SUCCESS(status) && transferred != Request->PayloadLength) {
        EMU_LOG_WARN(STR_MODULENAME "class request cs 0x%02X unit 0x%02X returned "
                     "%lu of %lu payload bytes\n",
                     Request->Bytes[3], Request->Bytes[5],
                     transferred, Request->PayloadLength);
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return status;
}

// Read the clock-rate control the descriptor named. It changes nothing, and it
// is the first class request this driver sends on its own, so the hardware run
// gets an answer for the endpoint-zero path before anything depends on it. A
// failure is not a start failure: v1 records that GET_CUR stalls on this
// hardware (docs/PROTOCOL.md), and a device that will not be read still has to
// work.
//
// The answer is also the cache's only feed for now. Whether this line reports a
// rate or a stall is the fork the state panel depends on: if the device answers,
// the driver can show what it read; if it does not, the cached values can only
// ever come from a SET_CUR this driver sent itself.
#pragma code_seg("PAGE")
static VOID
EmuUsbProbeClockRate(
    _In_ PEMU_DEVICE_CONTEXT Ctx
    )
{
    EMU_CTRL_REQUEST request;
    UCHAR   index = 0;
    ULONG   unitId;
    ULONG   rate;
    BOOLEAN noted;
    NTSTATUS status;

    PAGED_CODE();

    unitId = EmuCapsXuUnitId(Ctx->Caps, EMU_XU_CLOCK_RATE);
    if (unitId == EMU_NO_VALUE) {
        EMU_LOG_WARN(STR_MODULENAME "no clock-rate unit, control path not probed\n");
        return;
    }

    status = EmuCtrlBuildRequest(&request, FALSE, unitId,
                                 Ctx->Caps->AcInterfaceNumber,
                                 EMU_XU_CS_CLOCK_RATE_SELECT, 0, 1);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "clock-rate request rejected, 0x%x (unit 0x%02X)\n",
                      status, unitId);
        return;
    }

    rate = 0;
    status = EmuUsbSendControl(Ctx, &request, &index);
    EmuNoteProbeResult(Ctx, status, NT_SUCCESS(status)
                                         ? EmuCtrlRateFromIndex(index) : 0);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_TERSE(STR_MODULENAME "GET_CUR clock rate unit 0x%02X: 0x%x\n",
                      unitId, status);
        return;
    }
    rate = EmuCtrlRateFromIndex(index);
    // The cache now has readers besides this start path: a panel handle held
    // open across a stop/start can run GET_STATE at the same moment. Note is a
    // pure-memory write, so the lock is the whole story.
    ExAcquireFastMutex(&Ctx->StateLock);
    noted = EmuSettingsNoteRate(&Ctx->Settings, Ctx->Caps, rate);
    ExReleaseFastMutex(&Ctx->StateLock);
    if (!noted) {
        EMU_LOG_WARN(STR_MODULENAME "GET_CUR clock rate unit 0x%02X: index %u decodes "
                     "to %lu Hz, which the descriptor does not carry; cache unchanged\n",
                     unitId, (unsigned)index, rate);
        return;
    }
    EMU_LOG_TERSE(STR_MODULENAME "GET_CUR clock rate unit 0x%02X: index %u = %lu Hz, cached\n",
                  unitId, (unsigned)index, rate);
}

// Mirror the clock-rate probe outcome into the service key's Parameters
// values. The probe result decides whether the settings cache fills from reads
// or only from SET_CUR, and on a machine without working kernel-debug capture
// this registry breadcrumb is the only way to see the verdict. Write failures
// are ignored - diagnostics must not break the path they diagnose.
#pragma code_seg("PAGE")
VOID
EmuNoteProbeResult(
    _In_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ NTSTATUS            Status,
    _In_ ULONG               RateHz
    )
{
    HANDLE            keyHandle = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING    path;
    UNICODE_STRING    valueName;
    WCHAR             pathBuffer[512];
    WCHAR             statusName[] = L"LastProbeStatus";
    WCHAR             rateName[] = L"LastProbeRateHz";
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
    RtlInitUnicodeString(&valueName, statusName);
    ZwSetValueKey(keyHandle, &valueName, 0, REG_DWORD, &Status, sizeof(Status));
    RtlInitUnicodeString(&valueName, rateName);
    ZwSetValueKey(keyHandle, &valueName, 0, REG_DWORD, &RateHz, sizeof(RateHz));
    ZwClose(keyHandle);
}

#pragma code_seg("PAGE")
static VOID
EmuUsbLogCaps(_In_ const EMU_DEVICE_CAPS* caps)
{
    ULONG i;

    PAGED_CODE();

    EMU_LOG_TERSE(STR_MODULENAME "config %lu: %lu bytes, ac iface %lu, status ep 0x%02X, "
                  "%lu ifaces, %lu alts, %lu XU, %lu feature, %lu ch max, %lu-%lu Hz, "
                  "flags 0x%08x\n",
                  (ULONG)caps->ConfigurationValue, caps->BufferLength,
                  caps->AcInterfaceNumber, caps->StatusEndpoint,
                  caps->AudioInterfaceCount, caps->StreamAltCount,
                  caps->XuUnitCount, caps->FeatureUnitCount,
                  caps->MaxChannels, caps->MinRateHz, caps->MaxRateHz,
                  caps->ParsedFlags);

    for (i = 0; i < caps->AudioInterfaceCount; i++) {
        EMU_LOG_TERSE(STR_MODULENAME "iface %lu: class 0x%02X subclass 0x%02X role %lu\n",
                      caps->AudioInterfaces[i].InterfaceNumber,
                      caps->AudioInterfaces[i].DeclaredClass,
                      caps->AudioInterfaces[i].DeclaredSubclass,
                      caps->AudioInterfaces[i].Role);
    }
    for (i = 0; i < caps->StreamAltCount; i++) {
        const EMU_STREAM_ALT* alt = &caps->StreamAlts[i];
        EMU_LOG_TERSE(STR_MODULENAME "alt iface %lu setting %lu: dir %lu, %lu ch, "
                      "%lu/%lu bit, data ep 0x%02X/%lu B, fb ep 0x%02X/%lu B, %lu rates\n",
                      alt->InterfaceNumber, alt->AltSetting, alt->Direction,
                      alt->Channels, alt->SubFrameSize, alt->BitResolution,
                      alt->DataEndpoint, alt->DataMaxPacket,
                      alt->FeedbackEndpoint, alt->FeedbackMaxPacket,
                      alt->RateCount);
    }
    for (i = 0; i < caps->XuUnitCount; i++) {
        EMU_LOG_TERSE(STR_MODULENAME "XU unit 0x%02X code 0x%04X on iface %lu (layout %lu)\n",
                      caps->XuUnits[i].UnitID, caps->XuUnits[i].ExtensionCode,
                      caps->XuUnits[i].InterfaceNumber, caps->XuUnits[i].Layout);
    }
}

// Raw hex, 16 bytes per line. This is the capture path for the parser fixture:
// with TraceLevel >= 5 the whole descriptor comes out of a live 0404 and
// docs/descriptors can stop being a reconstruction. The level is a parameter
// on purpose: the bytes are the evidence for a parse failure too, and the
// failure branch prints them at ERROR so a machine whose filter mask does not
// pass BLAB still shows them.
#pragma code_seg("PAGE")
static VOID
EmuUsbDumpDescriptor(_In_ ULONG Level,
                     _In_reads_bytes_(Length) const UCHAR* Data,
                     _In_ ULONG Length)
{
    static const CHAR digits[] = "0123456789ABCDEF";
    ULONG    offset;
    ULONG    i;
    ULONG    count;
    BOOLEAN  first = TRUE;
    CHAR     line[3 * 16 + 1];

    PAGED_CODE();

    for (offset = 0; offset < Length; offset += 16) {
        count = Length - offset;
        if (count > 16) {
            count = 16;
        }
        for (i = 0; i < count; i++) {
            line[i * 3] = digits[Data[offset + i] >> 4];
            line[i * 3 + 1] = digits[Data[offset + i] & 0x0F];
            line[i * 3 + 2] = ' ';
        }
        line[count * 3] = ANSI_NULL;
        if (first) {
            EMU_TRACE(Level, STR_MODULENAME "config descriptor (%lu bytes):\n", Length);
            first = FALSE;
        }
        EMU_TRACE(Level, STR_MODULENAME "  @%04lu %s\n", offset, line);
    }
}

NTSTATUS
EmuUsbInitialize(
    _In_ PEMU_DEVICE_CONTEXT Ctx
    )
{
    PUSB_CONFIGURATION_DESCRIPTOR header = NULL;
    ULONG   total = 0;
    NTSTATUS status;

    PAGED_CODE();

    if (Ctx->UsbdHandle != NULL) {
        // Start without an intervening release: the handle, the descriptor and
        // the caps are still valid, so nothing has to be re-fetched.
        return STATUS_SUCCESS;
    }

    // PASSIVE_LEVEL only, and before any other request to the USB stack.
    status = USBD_CreateHandle(Ctx->Fdo,
                               Ctx->Pdo,
                               USBD_CLIENT_CONTRACT_VERSION_602,
                               EMU_POOLTAG,
                               &Ctx->UsbdHandle);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "USBD_CreateHandle failed, 0x%x\n", status);
        return status;
    }

    header = (PUSB_CONFIGURATION_DESCRIPTOR)
        ExAllocatePool2(POOL_FLAG_NON_PAGED, EMU_CONFIG_HEADER_MIN_LENGTH, EMU_POOLTAG);
    if (header == NULL) {
        EmuUsbRelease(Ctx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = EmuUsbReadConfigDescriptor(Ctx, (PUCHAR)header, EMU_CONFIG_HEADER_MIN_LENGTH);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    if (header->bLength != EMU_CONFIG_HEADER_MIN_LENGTH ||
        header->bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE) {
        EMU_LOG_ERROR(STR_MODULENAME "config header is not one (%lu/0x%02X)\n",
                      (ULONG)header->bLength, (ULONG)header->bDescriptorType);
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }
    total = header->wTotalLength;
    if (total < EMU_CONFIG_HEADER_MIN_LENGTH || total > EMU_CONFIG_BYTES_MAX) {
        EMU_LOG_ERROR(STR_MODULENAME "config wTotalLength %lu out of range\n", total);
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }

    Ctx->ConfigDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)
        ExAllocatePool2(POOL_FLAG_NON_PAGED, total, EMU_POOLTAG);
    if (Ctx->ConfigDescriptor == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    Ctx->ConfigDescriptorLength = total;

    status = EmuUsbReadConfigDescriptor(Ctx, (PUCHAR)Ctx->ConfigDescriptor, total);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    if (Ctx->ConfigDescriptor->wTotalLength != (USHORT)total) {
        EMU_LOG_ERROR(STR_MODULENAME "config changed between reads (%lu vs %lu)\n",
                      (ULONG)Ctx->ConfigDescriptor->wTotalLength, total);
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }

    // Dumped as soon as the bytes are in, whatever the parser then says: the
    // point of this dump is to capture what a live device really sends, and a
    // rejecting parser must not hide the evidence.
    EmuUsbDumpDescriptor(EMU_LEVEL_BLAB, (const UCHAR*)Ctx->ConfigDescriptor, total);

    // The caps are published under the state lock, parse included: a panel
    // handle held open across a stop/start can run GET_CAPS while this start
    // rebuilds the table, and a half-parsed Caps is worse than none — the
    // reader either sees NULL (its own "not ready") or a complete table.
    ExAcquireFastMutex(&Ctx->StateLock);
    Ctx->Caps = (PEMU_DEVICE_CAPS)
        ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(EMU_DEVICE_CAPS), EMU_POOLTAG);
    if (Ctx->Caps == NULL) {
        ExReleaseFastMutex(&Ctx->StateLock);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    status = EmuParseUsbConfigDescriptor((const UCHAR*)Ctx->ConfigDescriptor, total, Ctx->Caps);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "descriptor parse failed, 0x%x (%lu bytes)\n",
                      status, total);
        // The failing bytes themselves, at ERROR level: whatever filter mask
        // showed the failure line shows the hex too.
        EmuUsbDumpDescriptor(EMU_LEVEL_ERROR, (const UCHAR*)Ctx->ConfigDescriptor, total);
        ExReleaseFastMutex(&Ctx->StateLock);
        goto Exit;
    }

    EmuSettingsInit(&Ctx->Settings);
    ExReleaseFastMutex(&Ctx->StateLock);
    EmuUsbLogCaps(Ctx->Caps);
    EmuUsbProbeClockRate(Ctx);

Exit:
    if (header != NULL) {
        ExFreePoolWithTag(header, EMU_POOLTAG);
    }
    if (!NT_SUCCESS(status)) {
        EmuUsbRelease(Ctx);
    }
    return status;
}

VOID
EmuUsbRelease(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    )
{
    PAGED_CODE();

    // Every field is cleared as it goes, so a second call after a partial
    // initialize can never double-free.
    if (Ctx->UsbdHandle != NULL) {
        // The PDO still exists at this point: STOP and REMOVE both run before
        // the PnP manager deletes it, which is why the handle closes here and
        // not earlier. Closed outside the state lock — USBD_CloseHandle wants
        // PASSIVE_LEVEL, the lock runs at APC.
        USBD_CloseHandle(Ctx->UsbdHandle);
        Ctx->UsbdHandle = NULL;
    }
    // The frees and the cache reset are one section under the state lock: a
    // panel handle held open across this stop can be inside GET_CAPS/GET_STATE
    // right now, and it may read either the old table or "no device" — never a
    // table that is being freed.
    ExAcquireFastMutex(&Ctx->StateLock);
    if (Ctx->Caps != NULL) {
        ExFreePoolWithTag(Ctx->Caps, EMU_POOLTAG);
        Ctx->Caps = NULL;
    }
    if (Ctx->ConfigDescriptor != NULL) {
        ExFreePoolWithTag(Ctx->ConfigDescriptor, EMU_POOLTAG);
        Ctx->ConfigDescriptor = NULL;
        Ctx->ConfigDescriptorLength = 0;
    }
    // The controls live in the device, and the device is no longer reachable,
    // so the cache goes back to "nothing is known" with it. Whatever a later
    // start finds out again comes from a read or from its own SET_CUR.
    EmuSettingsInit(&Ctx->Settings);
    ExReleaseFastMutex(&Ctx->StateLock);
}
