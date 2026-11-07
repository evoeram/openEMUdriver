// EmuUSBAudio — render WaveRT and topology miniports (M4).
// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef _EMU_WAVE_H_
#define _EMU_WAVE_H_

#include "emu.h"
#include <stdunk.h>
#include "NewDelete.h"

class CEmuWaveStream;

class CEmuAdapter :
    public IAdapterPowerManagement,
    public CUnknown
{
private:
    PEMU_DEVICE_CONTEXT m_Context;

public:
    DECLARE_STD_UNKNOWN();
    IMP_IAdapterPowerManagement;

    CEmuAdapter(_In_opt_ PUNKNOWN Outer, _In_ PEMU_DEVICE_CONTEXT Context)
        : CUnknown(Outer), m_Context(Context) {}
    ~CEmuAdapter() {}
};

class CEmuWaveMiniport :
    public IMiniportWaveRT,
    public CUnknown
{
private:
    PEMU_DEVICE_CONTEXT m_Context;
    LONG m_StreamAllocated;

public:
    DECLARE_STD_UNKNOWN();
    IMP_IMiniportWaveRT;

    CEmuWaveMiniport(_In_opt_ PUNKNOWN Outer, _In_ PEMU_DEVICE_CONTEXT Context)
        : CUnknown(Outer), m_Context(Context), m_StreamAllocated(0) {}
    ~CEmuWaveMiniport() {}

    NTSTATUS PropertyHandlerProposedFormat(_In_ PPCPROPERTY_REQUEST Request);
    NTSTATUS PropertyHandlerProposedFormat2(_In_ PPCPROPERTY_REQUEST Request);

private:
    friend class CEmuWaveStream;
};

class CEmuTopologyMiniport :
    public IMiniportTopology,
    public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    IMP_IMiniportTopology;

    CEmuTopologyMiniport(_In_opt_ PUNKNOWN Outer) : CUnknown(Outer) {}
    ~CEmuTopologyMiniport() {}
};

typedef struct _EMU_WAVE_NOTIFICATION {
    LIST_ENTRY Link;
    PKEVENT Event;
} EMU_WAVE_NOTIFICATION, *PEMU_WAVE_NOTIFICATION;

class CEmuWaveStream :
    public IMiniportWaveRTStreamNotification,
    public IMiniportWaveRTOutputStream,
    public CUnknown
{
private:
    CEmuWaveMiniport* m_Miniport;
    PEMU_DEVICE_CONTEXT m_Context;
    PPORTWAVERTSTREAM m_PortStream;
    PUCHAR m_Buffer;
    PMDL m_BufferMdl;
    ULONG m_BufferBytes;
    ULONG m_NotificationCount;
    ULONG m_NotificationFrames;
    KSSTATE m_State;
    ULONGLONG m_BaseFrames;
    ULONGLONG m_NextNotificationFrame;
    LIST_ENTRY m_NotificationList;
    KSPIN_LOCK m_NotificationLock;
    // Absolute ring-packet index the OS has last claimed written (wrap-aware
    // reconstruction of the cyclic SetWritePacket argument). The ISO worker
    // gates packet submission on it.
    ULONGLONG m_LastOsWriteAbs;

public:
    DECLARE_STD_UNKNOWN();
    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;
    IMP_IMiniportWaveRTOutputStream;

    CEmuWaveStream(_In_opt_ PUNKNOWN Outer);
    ~CEmuWaveStream();

    NTSTATUS Init(
        _In_ CEmuWaveMiniport* Miniport,
        _In_ PPORTWAVERTSTREAM PortStream
        );

    static NTSTATUS NTAPI FillUsbPacket(
        _Inout_ PVOID Context,
        _Out_writes_bytes_(Frames * 6) PUCHAR Destination,
        _In_ ULONG Frames,
        _In_ ULONGLONG LinearFramePosition
        );

    static VOID NTAPI UsbPacketCompleted(
        _Inout_ PVOID Context,
        _In_ ULONGLONG CompletedFrames
        );

private:
    ULONGLONG TotalCompletedFrames();
    ULONGLONG TotalSubmittedFrames();
    NTSTATUS AllocateBuffer(
        _In_ ULONG NotificationCount,
        _In_ ULONG RequestedSize,
        _Out_ PMDL* AudioBufferMdl,
        _Out_ PULONG ActualSize,
        _Out_ PULONG OffsetFromFirstPage,
        _Out_ MEMORY_CACHING_TYPE* CacheType
        );
    VOID FreeBuffer(_In_opt_ PMDL Mdl);
    VOID StopAndReset();
};

// Register/unregister the Wave + Topology endpoint pair. Both are PASSIVE_LEVEL.
NTSTATUS EmuWaveStart(
    _Inout_ PEMU_DEVICE_CONTEXT Context,
    _In_opt_ PIRP Irp,
    _In_opt_ PRESOURCELIST ResourceList
    );

VOID EmuWaveStop(_Inout_ PEMU_DEVICE_CONTEXT Context);

#endif // _EMU_WAVE_H_
