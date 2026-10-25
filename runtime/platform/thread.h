// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Host threads with a chosen stack size: the guest's threads (each guest
// call is a host call, so guest code needs a deep host stack) and the
// game's main thread. pthreads on macOS, iOS and Linux, exactly as the
// runtime always made them; _beginthreadex on Windows, where the size is a
// reservation (STACK_SIZE_PARAM_IS_A_RESERVATION: committed as it grows),
// so 64 MB costs address space, not memory. std::thread can't do either:
// it has no stack size, and its handle can't outlive a running thread.
#pragma once
#include <cstddef>

#ifdef _WIN32
#include <cstdint>
#else
#include <pthread.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace platform
{
#ifdef _WIN32
    using Thread = void*;  // the thread's HANDLE
#else
    using Thread = pthread_t;
#endif

    // Starts fn(arg) on a new thread with `stackSize` bytes of stack.
    // Returns 0, or the error (pthread_create's; Windows: GetLastError()).
    int StartThread(Thread* thread, size_t stackSize, void* (*fn)(void*), void* arg);
    // Lets the thread run on its own: nothing joins it, its resources go
    // when it ends.
    void DetachThread(Thread thread);
    // Waits for the thread to end (then its handle is gone).
    void JoinThread(Thread thread);
    // The calling thread's name, for debuggers and profilers (Linux keeps
    // 15 characters; Windows: SetThreadDescription, where it exists).
    void NameCurrentThread(const char* name);

#ifndef _WIN32
    inline int StartThread(Thread* thread, size_t stackSize, void* (*fn)(void*), void* arg)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, stackSize);
        return pthread_create(thread, &attr, fn, arg);
    }

    inline void DetachThread(Thread thread)
    {
        pthread_detach(thread);
    }

    inline void JoinThread(Thread thread)
    {
        pthread_join(thread, nullptr);
    }

    inline void NameCurrentThread(const char* name)
    {
#if defined(__APPLE__)
        pthread_setname_np(name);
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), name);
#else
        (void)name;
#endif
    }
#endif
}
