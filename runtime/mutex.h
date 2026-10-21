// Adapted from Unleashed Recompiled (https://github.com/hedge-dev/UnleashedRecomp),
// GPL-3.0-or-later. Modified for SpeedBreaker.
#pragma once

#if 0  // SpeedBreaker Windows port: pthread-based Mutex below

struct Mutex : CRITICAL_SECTION
{
    Mutex()
    {
        InitializeCriticalSection(this);
    }
    ~Mutex()
    {
        DeleteCriticalSection(this);
    }

    void lock()
    {
        EnterCriticalSection(this);
    }

    void unlock()
    {
        LeaveCriticalSection(this);
    }
};

#else

using Mutex = std::mutex;

#endif
