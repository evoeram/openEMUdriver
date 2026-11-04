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
NTSTATUS
EmuUsbSendControl(
    _In_ PEMU_DEVICE_CONTEXT Context,
    _In_ const EMU_CTRL_REQUEST* Request,
    _Inout_opt_ PUCHAR Payload
    );

#endif // _EMU_USB_AUDIO_H_
