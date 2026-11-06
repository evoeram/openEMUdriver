// EmuUSBAudio — tagged C++ allocation for the native PortCls miniports.
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "emu.h"
#include <stdunk.h>
#include "NewDelete.h"

PVOID operator new(size_t Size, POOL_FLAGS PoolFlags, ULONG Tag)
{
    return ExAllocatePool2(PoolFlags, Size, Tag);
}

PVOID operator new(size_t Size, POOL_FLAGS PoolFlags)
{
    return ExAllocatePool2(PoolFlags, Size, EMU_POOLTAG);
}

static VOID EmuCppFree(_In_opt_ PVOID Pointer)
{
    if (Pointer != NULL) {
        ExFreePoolWithTag(Pointer, EMU_POOLTAG);
    }
}

// stdunk.lib already supplies the unsized scalar operator delete. Provide the
// sized/array forms here so compiler-selected deallocation keeps our pool tag.
void __cdecl operator delete(PVOID Pointer, size_t Size)
{
    UNREFERENCED_PARAMETER(Size);
    EmuCppFree(Pointer);
}

void __cdecl operator delete(PVOID Pointer, ULONG Tag)
{
    UNREFERENCED_PARAMETER(Tag);
    EmuCppFree(Pointer);
}

void __cdecl operator delete[](PVOID Pointer)
{
    EmuCppFree(Pointer);
}

void __cdecl operator delete[](PVOID Pointer, size_t Size)
{
    UNREFERENCED_PARAMETER(Size);
    EmuCppFree(Pointer);
}
