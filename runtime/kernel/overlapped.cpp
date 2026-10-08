// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include "overlapped.h"
#include "dispatcher.h"

void CompleteOverlapped(XXOVERLAPPED* overlapped, uint32_t result, uint32_t extendedError, uint32_t length)
{
    overlapped->Error = result;
    overlapped->dwExtendedError = extendedError;
    overlapped->Length = length;
    uint32_t event = overlapped->hEvent;
    if (event != 0 && IsKernelObject(event))
        if (auto* e = dynamic_cast<EventObject*>(GetKernelObject(event)))
            e->Set();
    if (overlapped->pCompletionRoutine != 0)
    {
        fprintf(stderr, "[xam] overlapped completion routine %08X is not supported yet\n",
            uint32_t(overlapped->pCompletionRoutine));
        assert(false && "overlapped completion routines are not implemented");
    }
}
