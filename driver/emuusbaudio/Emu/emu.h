// EmuUSBAudio — openEMUdriver v2 kernel driver.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// PortCLS adapter for the E-MU USB audio interfaces. M1 was a PnP skeleton; M2
// adds the contract-602 USB client (emu_usb.cpp), the descriptor parser
// (emu_desc.cpp) it feeds, the settings cache (emu_state.cpp) that parser fills
// and the panel contract (emu_ioctl.h) served by emu_disp.cpp. Still no KS
// filters and no audio paths.
#ifndef _EMU_USB_AUDIO_H_
#define _EMU_USB_AUDIO_H_

#include <portcls.h>
#include <usb.h>
// usbdex.lib exports plain C names, so a C++ translation unit has to take the
// declarations with C linkage — otherwise the link looks for mangled USBD_*
// symbols and finds none.
extern "C" {
#include <usbdlib.h>
}
#include "emu_desc.h"
#include "emu_ctrl.h"
#include "emu_state.h"
#include "emu_ioctl.h"
#include "emu_isoch.h"

#define STR_MODULENAME "EmuUSBAudio: "

// Pool tag for every allocation the driver makes.
#define EMU_POOLTAG 'Aemu'

// Upper bound on PcRegisterSubdevice() calls; M1 registers none, the value only
// sizes the PortCls subdevice table.
#define EMU_MAX_SUBDEVICES 2

// Tracing. ksdebug.h's DEBUGLVL_* macros are unusable here (evntrace.h hides
// TRACE_LEVEL_* from kernel-mode translation units and _DbgPrintF compiles away
// in free builds), so the levels are spelled out and gated on a module global:
// set Parameters\TraceLevel in the service key, or
// `ed EmuUSBAudio!EmuTraceLevel 5` from a kernel debugger.
#define EMU_LEVEL_ERROR   2   // TRACE_LEVEL_ERROR
#define EMU_LEVEL_WARN    3   // TRACE_LEVEL_WARNING
#define EMU_LEVEL_TERSE   4   // TRACE_LEVEL_INFORMATION: lifecycle events
#define EMU_LEVEL_BLAB    5   // TRACE_LEVEL_VERBOSE

#define EMU_TRACE(Level, ...)                                                 \
    do {                                                                      \
        if ((ULONG)(Level) <= EmuTraceLevel) {                                \
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, (Level), __VA_ARGS__);            \
        }                                                                     \
    } while (FALSE)

#define EMU_LOG_ERROR(...) EMU_TRACE(EMU_LEVEL_ERROR, __VA_ARGS__)
#define EMU_LOG_WARN(...)  EMU_TRACE(EMU_LEVEL_WARN,  __VA_ARGS__)
#define EMU_LOG_TERSE(...) EMU_TRACE(EMU_LEVEL_TERSE, __VA_ARGS__)
#define EMU_LOG_BLAB(...)  EMU_TRACE(EMU_LEVEL_BLAB,  __VA_ARGS__)

extern ULONG EmuTraceLevel;

// The service key path, copied out of DriverEntry; the diagnostics breadcrumb
// writes into its Parameters subkey when kernel debug capture is unavailable.
extern UNICODE_STRING EmuDriverRegistryPath;

// PnP state of one device instance. Stored as a LONG and written with
// InterlockedExchange so later stages (IOCTLs, stream threads) can read it
// without taking a lock.
typedef enum _EMU_STATE {
    EmuStateNotStarted = 0,
    EmuStateStarted,
    EmuStateStopped,
    EmuStateSurpriseRemoved,
    EmuStateRemoved
} EMU_STATE;

// The state travels to the panel as EMU_WIRE_STATE_* numbers, and the numbers
// are only as honest as this mapping. The driver tracks EMU_STATE, the panel
// reads EMU_WIRE_STATE_*; if either table is reordered, the handler that
// reports (LONG)State as the wire value starts lying in a way no runtime test
// can catch cheaply.
C_ASSERT((ULONG)EmuStateNotStarted      == EMU_WIRE_STATE_NOT_STARTED);
C_ASSERT((ULONG)EmuStateStarted         == EMU_WIRE_STATE_STARTED);
C_ASSERT((ULONG)EmuStateStopped         == EMU_WIRE_STATE_STOPPED);
C_ASSERT((ULONG)EmuStateSurpriseRemoved == EMU_WIRE_STATE_SURPRISE_REMOVED);
C_ASSERT((ULONG)EmuStateRemoved         == EMU_WIRE_STATE_REMOVED);

// Layout of the device extension PortCls creates for the FDO.
// PORT_CLASS_DEVICE_EXTENSION_SIZE is the whole PortCls-owned region
// (64 pointers); of its first 64 bytes the documented free slots are the four
// ULONG_PTR entries at offsets 32..63. We only read the PDO pointer PortCls
// recorded, to confirm this layout before touching our own region behind it.
typedef struct _PORT_CLASS_EXTENSION {
    ULONG_PTR      Reserved1[2];             // 0-15   PortCls
    PDEVICE_OBJECT PhysicalDeviceObject;     // 16-23  PortCls, read-only for us
    PVOID          Reserved2;                // 24-31  PortCls
    PVOID          AdapterDriverSlots[4];    // 32-63  documented as the adapter's
} PORT_CLASS_EXTENSION, *PPORT_CLASS_EXTENSION;

C_ASSERT(sizeof(PORT_CLASS_EXTENSION) == 64);

// Our per-instance context, in the bytes PcAddAdapterDevice appends after
// PORT_CLASS_DEVICE_EXTENSION_SIZE. IoCreateDevice zeroes the extension, so a
// fresh instance reads Signature == 0.
#define EMU_CTX_SIGNATURE 0x43656D75 // 'uCem'


typedef struct _EMU_DEVICE_CONTEXT {
    ULONG          Signature;
    LONG           State;           // EMU_STATE, read/written interlocked
    LONG           RemoveLockReady; // nonzero once RemoveLock is initialized
    ULONG          StartCount;
    ULONG          StopCount;
    ULONG          RemoveCount;
    PDEVICE_OBJECT Fdo;
    PDEVICE_OBJECT Pdo;
    IO_REMOVE_LOCK RemoveLock;

    // Guards Caps and Settings against the stop/start races the remove lock
    // does not cover: a panel handle left open across a disable/enable cycle
    // can still be reading caps while EmuUsbRelease frees them, and the remove
    // lock does not stop a STOP — it only stops a REMOVE. Every writer (start's
    // parse and probe, release) and every reader that fills an IOCTL reply
    // holds this mutex; it lives at APC_LEVEL for microseconds at a time, and
    // the PnP manager serializes START against STOP on top of it.
    FAST_MUTEX     StateLock;

    // M2 USB client state. The handle registers this driver with the USB stack
    // under contract 602; the descriptor block and the parsed caps are pool
    // allocations — EMU_DEVICE_CAPS is a few kilobytes and the PortCls
    // extension is nonpaged. All three are released together by EmuUsbRelease,
    // which is idempotent because stop/start can repeat without a remove.
    USBD_HANDLE                   UsbdHandle;
    PUSB_CONFIGURATION_DESCRIPTOR ConfigDescriptor;
    ULONG                         ConfigDescriptorLength;
    PEMU_DEVICE_CAPS              Caps;
    // What the driver has actually seen about the four E-MU controls. Small
    // enough to live in the extension, unlike Caps; EmuSettingsInit is the only
    // writer of the "nothing is known" state, and EmuUsbRelease calls it because
    // a released device leaves no evidence behind.
    EMU_SETTINGS                  Settings;
    // NTSTATUS of the last SET the panel attempted (0 = none yet). Written
    // under StateLock next to the cache it describes, reset on release:
    // after the device is gone the operation belongs to a dead session.
    NTSTATUS                      LastOperationStatus;

    // M3 stage A: the configured configuration. The select-config URB stays
    // alive for the whole start because the interface information - and
    // through it the pipe handles of whatever alternates get selected -
    // lives inside that allocation. The per-direction streaming entries
    // hold the select-interface URB of the currently selected alternate
    // (alt 0 = idle) and grow into the iso ring in phase B. Index by
    // EMU_STREAM_RENDER (0) / EMU_STREAM_CAPTURE (1).
    PURB                          SelectConfigUrb;
    USBD_CONFIGURATION_HANDLE     ConfigHandle;
    struct {
        PURB                      SelectInterfaceUrb;
        PUSBD_INTERFACE_INFORMATION Interface;
        ULONG                     InterfaceNumber;
        ULONG                     AltSetting;
    }                             Stream[2];

    // M3 stage B: the test-slice iso streams, one engine state per direction.
    // Embedded rather than pointed: the whole state is bounded, nonpaged by
    // usage and dies with the instance. EmuIsochStartStreams arms them, the
    // test IOCTLs and EmuUsbRelease (via EmuIsochStopAll) retire them.
    EMU_ISOCH_STREAM              Isoch[2];

    // M4 render endpoint. These are owning references returned by QueryInterface;
    // the registration flags distinguish a created port from a registered KS
    // subdevice so failed starts and repeated PnP stops share one teardown path.
    PUNKNOWN                      AdapterUnknown;
    PUNKNOWN                      WavePort;
    PUNKNOWN                      TopologyPort;
    BOOLEAN                       WavePortRegistered;
    BOOLEAN                       TopologyPortRegistered;
    BOOLEAN                       PhysicalConnectionRegistered;
} EMU_DEVICE_CONTEXT, *PEMU_DEVICE_CONTEXT;

// Stage 4c: the panel talks to a driver-owned control device object, not to
// the FDO. Hardware run 2026-11-04 proved that KS/PortCls rejects
// IRP_MJ_CREATE on the adapter FDO (no create item matches an empty file
// name), so the interface on the PDO could never be opened. The control
// device carries this signature in its first extension ULONG, which is how
// the shared dispatch tells it apart from the FDO (whose extension starts
// with the PortCls region).
#define EMU_CTL_SIGNATURE 0x4C74436D // 'mCtL'

// The started adapter instance, published for the control device's dispatch.
// Set at the end of a successful start, cleared on stop and remove; read
// atomically, and guarded by the FDO's remove lock plus an object reference
// while in use (a panel handle on the control device must not outlive the
// context it reads).
extern PEMU_DEVICE_CONTEXT EmuControlAdapter;

#define EMU_CONTEXT(Fdo) \
    ((PEMU_DEVICE_CONTEXT)((PCHAR)(Fdo)->DeviceExtension + PORT_CLASS_DEVICE_EXTENSION_SIZE))

// emu_driver.cpp
extern "C" DRIVER_INITIALIZE DriverEntry;

// emu_pnp.cpp
extern "C" DRIVER_ADD_DEVICE AddDevice;
extern "C" DRIVER_DISPATCH   EmuPnpHandler;

// emu_disp.cpp — explicit prototypes, not DRIVER_DISPATCH declarations, per
// the M1 note about typedef-typed declarations: a function type typedef makes
// the bare form work, a pointer typedef silently declares a variable instead.
extern "C" NTSTATUS NTAPI EmuDeviceControlHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp
    );

extern "C" NTSTATUS NTAPI EmuCreateCloseHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP        Irp
    );

extern "C" NTSTATUS
EmuStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp,
    _In_ PRESOURCELIST  ResourceList
    );

// emu_usb.cpp — contract-602 USB client. All three are PASSIVE_LEVEL and paged;
// EmuUsbRelease is idempotent.
NTSTATUS
EmuUsbInitialize(
    _In_ PEMU_DEVICE_CONTEXT Context
    );

VOID
EmuUsbRelease(
    _Inout_ PEMU_DEVICE_CONTEXT Context
    );

// Submit one built class request on endpoint zero and wait for it. Payload is
// the data stage: filled for a device-to-host request, sent for a
// host-to-device one, NULL when there is none.
// Execute one SET operation end to end: build the CA0189 class request for
// the contract action, send it on endpoint zero, note the accepted value in
// the cache and record the outcome for GET_STATE. PASSIVE_LEVEL only - the
// send waits on the bus - and the whole operation runs under StateLock, so
// SET against SET and SET against a stop are serialized and GET_STATE
// readers wait for a control transfer (milliseconds) instead of seeing a
// half-applied change. The descriptor gets a veto before anything reaches
// the wire: a rate it does not carry, or a SET for a unit the device does
// not have, is refused without a bus roundtrip.
// Configuration and interface selection (M3 stage A). SelectConfiguration
// runs once per start and parks the configuration handle plus the alt-0
// interface information in the select-config URB; SelectInterface moves one
// direction to a streaming alternate and back (alt 0 = idle), keeping that
// direction's select-interface URB alive while the alternate is
// selected. Both are PASSIVE_LEVEL and breadcrumb their outcomes into
// Parameters (SelConfigStatus / SelRenderStatus / SelCaptureStatus /
// SelIdleStatus) - the USBD_INTERFACE_LIST_ENTRY semantics were a plan
// question until this run proves them on hardware.
NTSTATUS
EmuUsbSelectConfiguration(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    );

NTSTATUS
EmuUsbSelectInterface(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ ULONG                  Direction,
    _In_ ULONG                  InterfaceNumber,
    _In_ ULONG                  AltSetting
    );

// The per-start diagnostic pass: render and capture move to their 48 kHz
// alternates and back, every step mirrored into the registry breadcrumb.
VOID
EmuUsbProbeAltSelection(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx
    );

// One named DWORD into the service Parameters key - the diagnostic channel
// of record on this machine (see EmuNoteProbeResult in emu_usb.cpp). Driver
// Verifier, while enabled, blocks these writes from DriverEntry.
VOID
EmuNoteStatusValue(
    _In_opt_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ PCWSTR              Name,
    _In_ ULONG               Value
    );

// One named DWORD into the service Parameters key - the diagnostic channel
// of record on this machine (see EmuNoteProbeResult).

// The panel-SET entry point: takes StateLock itself. Callers that already
// hold the lock (the test-stream branch of the dispatch) must use the Locked
// variant below - a fast mutex is not recursive, and a second acquire on the
// same thread waits for itself forever.
NTSTATUS
EmuUsbExecuteSet(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ EMU_IOCTL_ACTION       Action,
    _In_ ULONG                  Value
    );

// The same SET executor with StateLock already held by the caller. The SET
// body runs bus operations while holding it; the lock serializes SETs, test
// starts and stops against each other, which is exactly what the test branch
// needs for its restart semantics.
NTSTATUS
EmuUsbExecuteSetLocked(
    _Inout_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ EMU_IOCTL_ACTION       Action,
    _In_ ULONG                  Value
    );

VOID
EmuNoteProbeResult(
    _In_ PEMU_DEVICE_CONTEXT Ctx,
    _In_ NTSTATUS            Status,
    _In_ ULONG               RateHz
    );

NTSTATUS
EmuUsbSendControl(
    _In_ PEMU_DEVICE_CONTEXT Context,
    _In_ const EMU_CTRL_REQUEST* Request,
    _Inout_opt_ PUCHAR Payload
    );

// Submit one built URB synchronously and return its status. The iso engine
// (emu_isoch.cpp) uses it for the frame-number query and the pipe abort; the
// engine's own packet submissions manage their IRPs directly.
NTSTATUS
EmuUsbSubmitUrb(
    _In_ PDEVICE_OBJECT Target,
    _In_ USBD_HANDLE    Handle,
    _In_ PURB           Urb
    );

// emu_wave.cpp — M4 render WaveRT + topology endpoint pair.
NTSTATUS
EmuWaveStart(
    _Inout_ PEMU_DEVICE_CONTEXT Context,
    _In_opt_ PIRP Irp,
    _In_opt_ PRESOURCELIST ResourceList
    );

VOID
EmuWaveStop(
    _Inout_ PEMU_DEVICE_CONTEXT Context
    );

#endif // _EMU_USB_AUDIO_H_
