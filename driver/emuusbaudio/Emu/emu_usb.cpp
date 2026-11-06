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

NTSTATUS
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

NTSTATUS
EmuUsbExecuteSet(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ EMU_IOCTL_ACTION       Action,
    _In_ ULONG                  Value
    )
{
    NTSTATUS status;

    PAGED_CODE();

    ExAcquireFastMutex(&Ctx->StateLock);
    status = EmuUsbExecuteSetLocked(Ctx, Action, Value);
    ExReleaseFastMutex(&Ctx->StateLock);
    return status;
}

// The SET body proper. The caller owns StateLock: the panel SET branch of the
// dispatch calls this through EmuUsbExecuteSet, the test-stream branch holds
// the lock across the whole start/stop and comes straight here. A fast mutex
// is not recursive - the second ExAcquireFastMutex on the same thread would
// wait on itself forever, which is exactly what the first hardware run of the
// test start did (the probe stuck in TEST_STREAM_START, TestStartStatus
// unwritten).
#pragma code_seg("PAGE")
NTSTATUS
EmuUsbExecuteSetLocked(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ EMU_IOCTL_ACTION       Action,
    _In_ ULONG                  Value
    )
{
    EMU_CTRL_REQUEST request;
    ULONG            unitId;
    NTSTATUS         status;

    PAGED_CODE();


    switch (Action) {
    case EmuIoctlSetRate:
        // The descriptor gets a veto before anything reaches the wire: a rate
        // it does not carry is refused without a bus roundtrip.
        if (!EmuCapsSupportsRate(Ctx->Caps, Value)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        unitId = EmuCapsXuUnitId(Ctx->Caps, EMU_XU_CLOCK_RATE);
        status = EmuCtrlBuildClockRateSet(&request, unitId,
                                          Ctx->Caps->AcInterfaceNumber, Value);
        if (!NT_SUCCESS(status)) {
            break;
        }
        status = EmuUsbSendControl(Ctx, &request, request.Payload);
        if (NT_SUCCESS(status)) {
            (VOID)EmuSettingsNoteRate(&Ctx->Settings, Ctx->Caps, Value);
        }
        break;

    case EmuIoctlSetClockSource:
        unitId = EmuCapsXuUnitId(Ctx->Caps, EMU_XU_CLOCK_SOURCE);
        status = EmuCtrlBuildClockSourceSet(&request, unitId,
                                            Ctx->Caps->AcInterfaceNumber, Value);
        if (!NT_SUCCESS(status)) {
            break;
        }
        status = EmuUsbSendControl(Ctx, &request, request.Payload);
        if (NT_SUCCESS(status)) {
            (VOID)EmuSettingsNoteClockSource(&Ctx->Settings, Value);
        }
        break;

    case EmuIoctlSetSpdifFormat:
        unitId = EmuCapsXuUnitId(Ctx->Caps, EMU_XU_DIGITAL_IO);
        status = EmuCtrlBuildSpdifFormatSet(&request, unitId,
                                            Ctx->Caps->AcInterfaceNumber, Value);
        if (!NT_SUCCESS(status)) {
            break;
        }
        status = EmuUsbSendControl(Ctx, &request, request.Payload);
        if (NT_SUCCESS(status)) {
            (VOID)EmuSettingsNoteSpdifFormat(&Ctx->Settings, Value);
        }
        break;

    case EmuIoctlSetSoftLimit:
        unitId = EmuCapsXuUnitId(Ctx->Caps, EMU_XU_DEVICE_OPTIONS);
        status = EmuCtrlBuildSoftLimitSet(&request, unitId,
                                          Ctx->Caps->AcInterfaceNumber,
                                          Value != 0 ? TRUE : FALSE);
        if (!NT_SUCCESS(status)) {
            break;
        }
        status = EmuUsbSendControl(Ctx, &request, request.Payload);
        if (NT_SUCCESS(status)) {
            (VOID)EmuSettingsNoteSoftLimit(&Ctx->Settings, Value);
        }
        break;

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    Ctx->LastOperationStatus = status;
    return status;
}

// Mirror the clock-rate probe outcome into the service key's Parameters
// values. The probe result decides whether the settings cache fills from reads
// or only from SET_CUR, and on a machine without working kernel-debug capture
// this registry breadcrumb is the only way to see the verdict. Write failures
// are ignored - diagnostics must not break the path they diagnose.
#pragma code_seg("PAGE")
VOID
EmuNoteStatusValue(
    _In_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ PCWSTR              Name,
    _In_ ULONG               Value
    )
{
    HANDLE            keyHandle = NULL;
    OBJECT_ATTRIBUTES objectAttributes;
    UNICODE_STRING    path;
    UNICODE_STRING    valueName;
    WCHAR             pathBuffer[512];

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Ctx);

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
    RtlInitUnicodeString(&valueName, Name);
    ZwSetValueKey(keyHandle, &valueName, 0, REG_DWORD, &Value, sizeof(Value));
    ZwClose(keyHandle);
}

VOID
EmuNoteProbeResult(
    _In_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ NTSTATUS            Status,
    _In_ ULONG               RateHz
    )
{
    PAGED_CODE();
    EmuNoteStatusValue(Ctx, L"LastProbeStatus", (ULONG)Status);
    EmuNoteStatusValue(Ctx, L"LastProbeRateHz", RateHz);
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

    // M3 stage A: configure the configuration. Without it there is no
    // configuration handle and no pipe handles, which M3's streaming needs.
    status = EmuUsbSelectConfiguration(Ctx);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    EmuUsbProbeAltSelection(Ctx);

Exit:
    if (header != NULL) {
        ExFreePoolWithTag(header, EMU_POOLTAG);
    }
    if (!NT_SUCCESS(status)) {
        EmuUsbRelease(Ctx);
    }
    return status;
}

// Configure the configuration: one interface list entry per audio interface,
// all at alternate setting 0 (idle). The entries point into our own
// configuration descriptor copy, which stays alive for the whole start; the
// interface information USBD fills in lives inside the select-config URB,
// which stays alive too. The URB is submitted here: a build-only pass left
// the configuration handle NULL (only the stack assigns it, during submit)
// and made the M3-A "SelectConfiguration succeeded" verdict a false positive.
#pragma code_seg("PAGE")
NTSTATUS
EmuUsbSelectConfiguration(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    )
{
    PUSBD_INTERFACE_LIST_ENTRY list;
    NTSTATUS status;

    PAGED_CODE();
    EmuNoteStatusValue(Ctx, L"SelConfigEnter", 1);

    ULONG count = Ctx->Caps->AudioInterfaceCount;
    list = (PUSBD_INTERFACE_LIST_ENTRY)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        (count + 1) * sizeof(USBD_INTERFACE_LIST_ENTRY),
        EMU_POOLTAG);
    if (list == NULL) {
        EmuNoteStatusValue(Ctx, L"SelConfigStatus",
                           (ULONG)STATUS_INSUFFICIENT_RESOURCES);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (ULONG i = 0; i < count; i++) {
        LONG off = EmuCapsFindInterfaceDescriptor(
            (const UCHAR*)Ctx->ConfigDescriptor, Ctx->ConfigDescriptorLength,
            Ctx->Caps->AudioInterfaces[i].InterfaceNumber, 0);
        if (off < 0) {
            // The parser accepted an interface the block no longer carries:
            // refuse rather than hand USBD a pointer into nowhere.
            ExFreePoolWithTag(list, EMU_POOLTAG);
            EmuNoteStatusValue(Ctx, L"SelConfigStatus",
                               (ULONG)STATUS_DEVICE_CONFIGURATION_ERROR);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        list[i].InterfaceDescriptor =
            (PUSB_INTERFACE_DESCRIPTOR)&((PUCHAR)Ctx->ConfigDescriptor)[off];
        list[i].Interface = NULL;
    }
    list[count].InterfaceDescriptor = NULL;
    list[count].Interface = NULL;

    status = USBD_SelectConfigUrbAllocateAndBuild(Ctx->UsbdHandle,
                                                  Ctx->ConfigDescriptor,
                                                  list,
                                                  &Ctx->SelectConfigUrb);
    ExFreePoolWithTag(list, EMU_POOLTAG);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "select configuration build failed, "
                      "0x%x\n", status);
        EmuNoteStatusValue(Ctx, L"SelConfigStatus", (ULONG)status);
        return status;
    }

    // The built URB is not the selection: the SET_CONFIGURATION runs, and the
    // configuration handle is assigned, during submission. An unsubmitted
    // build left ConfigHandle NULL, which the select-interface wrapper then
    // refused with STATUS_INVALID_PARAMETER - the M3-A 0xC000000D.
    status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, Ctx->SelectConfigUrb);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "select configuration submit failed, "
                      "0x%x\n", status);
        EmuNoteStatusValue(Ctx, L"SelConfigStatus", (ULONG)status);
        return status;
    }

    // The configuration handle every select-interface URB needs, together
    // with the alt-0 interface information, exists only after the submit.
    Ctx->ConfigHandle = Ctx->SelectConfigUrb->UrbSelectConfiguration.ConfigurationHandle;
    if (Ctx->ConfigHandle == NULL) {
        // A successful submit without a handle breaks the stack contract;
        // refuse here rather than let select-interface's NULL check be the
        // first place the break is noticed.
        EMU_LOG_ERROR(STR_MODULENAME "select configuration returned no "
                      "handle\n");
        EmuNoteStatusValue(Ctx, L"SelConfigStatus",
                           (ULONG)STATUS_DEVICE_CONFIGURATION_ERROR);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    EmuNoteStatusValue(Ctx, L"SelConfigStatus", (ULONG)status);
    return status;
}

// Move one direction to a streaming alternate (alt 0 returns it to idle):
// build the select-interface URB and submit it, so the SET_INTERFACE runs
// and the pipe handles phase B needs become real. The submitted URB stays in
// the direction's slot - the interface information lives inside it. A
// re-selection frees the previous URB first, which is safe while nothing
// streams from its pipes.
#pragma code_seg("PAGE")
NTSTATUS
EmuUsbSelectInterface(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ ULONG                  Direction,
    _In_ ULONG                  InterfaceNumber,
    _In_ ULONG                  AltSetting
    )
{
    // Two entries: the interface being selected plus the {NULL, NULL}
    // terminator the USBD list walker requires even for a single
    // interface (the single-entry form was refused with
    // STATUS_INVALID_PARAMETER on hardware). Both live on the stack for
    // the duration of the call.
    USBD_INTERFACE_LIST_ENTRY entry[2];
    PURB urb = NULL;
    NTSTATUS status;

    PAGED_CODE();

    if (Direction > 1) {
        return STATUS_INVALID_PARAMETER;
    }
    LONG off = EmuCapsFindInterfaceDescriptor(
        (const UCHAR*)Ctx->ConfigDescriptor, Ctx->ConfigDescriptorLength,
        InterfaceNumber, AltSetting);
    if (off < 0) {
        return STATUS_INVALID_PARAMETER;
    }
    entry[0].InterfaceDescriptor =
        (PUSB_INTERFACE_DESCRIPTOR)&((PUCHAR)Ctx->ConfigDescriptor)[off];
    entry[0].Interface = NULL;
    entry[1].InterfaceDescriptor = NULL;
    entry[1].Interface = NULL;

    status = USBD_SelectInterfaceUrbAllocateAndBuild(Ctx->UsbdHandle,
                                                     Ctx->ConfigHandle,
                                                     entry,
                                                     &urb);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "select interface %lu alt %lu build "
                      "failed, 0x%x\n", InterfaceNumber, AltSetting, status);
        return status;
    }

    // The built URB is not the selection: the SET_INTERFACE runs, and the
    // pipe handles become real, during submission. A failed submit leaves
    // the previous alternate in place - the new URB is dropped, the stored
    // one untouched.
    status = EmuUsbSubmitUrb(Ctx->Pdo, Ctx->UsbdHandle, urb);
    if (!NT_SUCCESS(status)) {
        EMU_LOG_ERROR(STR_MODULENAME "select interface %lu alt %lu submit "
                      "failed, 0x%x\n", InterfaceNumber, AltSetting, status);
        USBD_UrbFree(Ctx->UsbdHandle, urb);
        return status;
    }

    if (Ctx->Stream[Direction].SelectInterfaceUrb != NULL) {
        USBD_UrbFree(Ctx->UsbdHandle, Ctx->Stream[Direction].SelectInterfaceUrb);
    }
    Ctx->Stream[Direction].SelectInterfaceUrb = urb;
    Ctx->Stream[Direction].Interface = &urb->UrbSelectInterface.Interface;
    Ctx->Stream[Direction].InterfaceNumber = InterfaceNumber;
    Ctx->Stream[Direction].AltSetting = AltSetting;
    return status;
}

// Diagnostic pass over the streaming alternates, run once per start: move
// render and capture to their 48 kHz alternates, then back to idle. Each step
// is breadrumbed, so a hardware run answers whether the interface-selection
// semantics work before M3 hangs an iso ring on them. No data is transferred:
// a selected OUT alternate with nothing sent is idle by definition, and an IN
// alternate with no pending transfer schedules nothing.
#pragma code_seg("PAGE")
VOID
EmuUsbProbeAltSelection(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    )
{
    const EMU_STREAM_ALT* render;
    const EMU_STREAM_ALT* capture;
    NTSTATUS status;

    PAGED_CODE();

    render = EmuCapsFindAlt(Ctx->Caps, EMU_STREAM_RENDER, 2, 48000);
    capture = EmuCapsFindAlt(Ctx->Caps, EMU_STREAM_CAPTURE, 2, 48000);
    if (render == NULL || capture == NULL) {
        // The descriptor vetted rates per alt already; without a 48 kHz pair
        // there is nothing for M3's first test to stream and nothing to probe.
        EMU_LOG_WARN(STR_MODULENAME "no 2ch 48 kHz alt pair, selection not probed\n");
        EmuNoteStatusValue(Ctx, L"SelConfigStatus", (ULONG)0);
        return;
    }

    status = EmuUsbSelectInterface(Ctx, EMU_STREAM_RENDER,
                                   render->InterfaceNumber, render->AltSetting);
    EmuNoteStatusValue(Ctx, L"SelRenderStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return;
    }

    status = EmuUsbSelectInterface(Ctx, EMU_STREAM_CAPTURE,
                                   capture->InterfaceNumber, capture->AltSetting);
    EmuNoteStatusValue(Ctx, L"SelCaptureStatus", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return;
    }

    // Back to idle: both directions at alternate setting 0. The idle status is
    // the last breadcrumb a reader sees unless a later step overwrites it.
    status = EmuUsbSelectInterface(Ctx, EMU_STREAM_RENDER,
                                   render->InterfaceNumber, 0);
    if (NT_SUCCESS(status)) {
        status = EmuUsbSelectInterface(Ctx, EMU_STREAM_CAPTURE,
                                       capture->InterfaceNumber, 0);
    }
    EmuNoteStatusValue(Ctx, L"SelIdleStatus", (ULONG)status);
}

VOID
EmuUsbRelease(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    )
{
    PAGED_CODE();

    // The test streams die first: their worker threads and queued iso URBs
    // reference the pipe handles the select-interface URBs below carry, and
    // the stop path drains both before anything here is freed. No bus
    // operation is attempted from here - on the release path the device may
    // already be gone, and the abort failing is the normal case there.
    EmuIsochStopAll(Ctx);

    // Every field is cleared as it goes, so a second call after a partial
    // initialize can never double-free.
    for (ULONG dir = 0; dir < 2; dir++) {
        if (Ctx->Stream[dir].SelectInterfaceUrb != NULL) {
            // The select-interface URB owns the interface information of the
            // currently selected alternate; with the device gone neither the
            // URB nor the alternate matter.
            USBD_UrbFree(Ctx->UsbdHandle, Ctx->Stream[dir].SelectInterfaceUrb);
            Ctx->Stream[dir].SelectInterfaceUrb = NULL;
        }
        Ctx->Stream[dir].Interface = NULL;
    }
    if (Ctx->SelectConfigUrb != NULL) {
        // The configuration handle and the alt-0 interface information live
        // inside this allocation - it dies with the start, like them.
        USBD_UrbFree(Ctx->UsbdHandle, Ctx->SelectConfigUrb);
        Ctx->SelectConfigUrb = NULL;
    }
    Ctx->ConfigHandle = NULL;
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
    // start finds out again comes from a read or from its own SET_CUR. The
    // last SET operation belongs to the dead session too.
    EmuSettingsInit(&Ctx->Settings);
    Ctx->LastOperationStatus = 0;
    ExReleaseFastMutex(&Ctx->StateLock);
}
