// EmuUSBAudio — openEMUdriver v2 kernel driver.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// PortCLS adapter for the E-MU USB audio interfaces. This is the M1 stage: a
// PnP skeleton only. No USB I/O, no KS filters, no audio paths — the whole
// point of M1 is a PortCls FDO that survives start/stop/remove/surprise-removal
// cycles under Driver Verifier without leaks or bugchecks.
#ifndef _EMU_USB_AUDIO_H_
#define _EMU_USB_AUDIO_H_

#include <portcls.h>

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
} EMU_DEVICE_CONTEXT, *PEMU_DEVICE_CONTEXT;

#define EMU_CONTEXT(Fdo) \
    ((PEMU_DEVICE_CONTEXT)((PCHAR)(Fdo)->DeviceExtension + PORT_CLASS_DEVICE_EXTENSION_SIZE))

// emu_driver.cpp
extern "C" DRIVER_INITIALIZE DriverEntry;

// emu_pnp.cpp
extern "C" DRIVER_ADD_DEVICE AddDevice;
extern "C" DRIVER_DISPATCH   EmuPnpHandler;

extern "C" NTSTATUS
EmuStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp,
    _In_ PRESOURCELIST  ResourceList
    );

#endif // _EMU_USB_AUDIO_H_
