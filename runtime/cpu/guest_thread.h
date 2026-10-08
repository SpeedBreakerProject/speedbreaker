// Adapted from Unleashed Recompiled (https://github.com/hedge-dev/UnleashedRecomp),
// GPL-3.0-or-later. Modified for SpeedBreaker.
#pragma once

#include <kernel/xdm.h>
#include <kernel/dispatcher.h>

// SpeedBreaker: pthreads on every platform (macOS and Linux), for the stack
// size and so thread handles share one detached, reference-counted path.
#define USE_PTHREAD 1

#ifdef USE_PTHREAD
#include <pthread.h>
#endif

#define CURRENT_THREAD_HANDLE uint32_t(-2)

struct GuestThreadContext
{
    PPCContext ppcContext{};
    uint8_t* thread = nullptr;

    GuestThreadContext(uint32_t cpuNumber);
    ~GuestThreadContext();
};

// Thrown by ExTerminateThread: unwinds the guest call stack back to
// GuestThread::Start (the 360 call never returns).
struct GuestThreadExit
{
    uint32_t exitCode;
};

struct GuestThreadParams
{
    uint32_t function;
    uint32_t value;   // r3
    uint32_t flags;
    uint32_t value2 = 0;  // r4 (SpeedBreaker: XAPI thread startup takes two arguments)
};

struct GuestThreadHandle : Waitable
{
    GuestThreadParams params;
    uint32_t id;
    std::atomic<bool> suspended;
    bool finished = false;  // SpeedBreaker: guarded by dispatcher::Lock()
#ifdef USE_PTHREAD
    pthread_t thread;
#else
    std::thread thread;
#endif

    GuestThreadHandle(const GuestThreadParams& params);
    ~GuestThreadHandle() override;

    uint32_t GetThreadId() const;

    // SpeedBreaker: signaled once the thread has finished.
    bool IsSignaledLocked() const override { return finished; }
};

struct GuestThread
{
    static uint32_t Start(const GuestThreadParams& params);
    static GuestThreadHandle* Start(const GuestThreadParams& params, uint32_t* threadId);

    static uint32_t GetCurrentThreadId();
    static void SetLastError(uint32_t error);
    static void SetDefaultStackSize(size_t size);
    // The current thread's kernel object (for the 0xFFFFFFFE pseudo-handle).
    static KernelObject* GetCurrentThreadObject();

#ifdef _WIN32
    static void SetThreadName(uint32_t threadId, const char* name);
#endif
};

// Names the calling host thread, for top -H, perf and debuggers (at most 15
// characters are kept on Linux).
void SetHostThreadName(const char* name);
