// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Object manager. Kernel objects live until NtClose destroys them (see
// xdm.h), so reference counting is not modelled yet: references taken with
// ObReferenceObjectByHandle / ObReferenceObject are simply not counted.
// If the game ever closes a handle while still holding a reference, this is
// where to add real counts.
#include <stdafx.h>
#include "function.h"
#include "xdm.h"
#include <cpu/guest_thread.h>

void ObDereferenceObject(uint32_t object) {}
void ObReferenceObject(uint32_t object) {}

uint32_t NtClose(uint32_t handle);

constexpr uint32_t CURRENT_PROCESS_PSEUDO = 0xFFFFFFFF;
constexpr uint32_t CURRENT_THREAD_PSEUDO = 0xFFFFFFFE;
constexpr uint32_t DUPLICATE_CLOSE_SOURCE = 0x1;

uint32_t NtDuplicateObject(uint32_t sourceHandle, be<uint32_t>* targetHandle, uint32_t options)
{
    KernelObject* obj;
    if (sourceHandle == CURRENT_THREAD_PSEUDO)
        obj = GuestThread::GetCurrentThreadObject();
    else if (sourceHandle != CURRENT_PROCESS_PSEUDO && IsKernelObject(sourceHandle))
        obj = GetKernelObject(sourceHandle);
    else
        return STATUS_INVALID_HANDLE;

    RetainKernelObject(obj);
    if (targetHandle)
        *targetHandle = GetKernelHandle(obj);
    if ((options & DUPLICATE_CLOSE_SOURCE) && sourceHandle != CURRENT_THREAD_PSEUDO)
        NtClose(sourceHandle);
    return STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__NtDuplicateObject, NtDuplicateObject);
GUEST_FUNCTION_HOOK(__imp__ObDereferenceObject, ObDereferenceObject);
GUEST_FUNCTION_HOOK(__imp__ObReferenceObject, ObReferenceObject);
