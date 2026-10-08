// Adapted from Unleashed Recompiled (https://github.com/hedge-dev/UnleashedRecomp),
// GPL-3.0-or-later. Modified for SpeedBreaker.
#include <stdafx.h>
#include "guest_thread.h"
#include <kernel/memory.h>
#include <kernel/heap.h>
#include <kernel/function.h>
#include <cpu/ppc_context.h>
#include <report/report.h>

#include <sys/resource.h>

constexpr size_t PCR_SIZE = 0xAB0;
constexpr size_t TLS_SIZE = 0x100;
constexpr size_t TEB_SIZE = 0x2E0;
// SpeedBreaker: guest stack size comes from the XEX (SetDefaultGuestStackSize).
static size_t STACK_SIZE = 0x40000;
void GuestThread::SetDefaultStackSize(size_t size) { STACK_SIZE = size; }
#define TOTAL_SIZE (PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE)

constexpr size_t TEB_OFFSET = PCR_SIZE + TLS_SIZE;

// SpeedBreaker: small sequential ids (no xxHash). Each handle owns its id and
// the new host thread adopts it, so the creator and the thread agree.
static std::atomic<uint32_t> s_nextThreadId{ 1 };
static thread_local uint32_t t_threadId = 0;
static thread_local KernelObject* t_self = nullptr;

// Threads not created through ExCreateThread (the main thread) get a plain
// object: nothing can wait for them to finish.
struct ForeignThreadObject final : Waitable
{
    bool IsSignaledLocked() const override { return false; }
};

KernelObject* GuestThread::GetCurrentThreadObject()
{
    if (t_self == nullptr)
        t_self = CreateKernelObject<ForeignThreadObject>();
    return t_self;
}

static uint32_t CurrentThreadId()
{
    if (t_threadId == 0)
        t_threadId = s_nextThreadId++;
    return t_threadId;
}


GuestThreadContext::GuestThreadContext(uint32_t cpuNumber)
{
    assert(thread == nullptr);

    thread = (uint8_t*)g_userHeap.Alloc(TOTAL_SIZE);
    memset(thread, 0, TOTAL_SIZE);

    *(uint32_t*)thread = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE)); // tls pointer
    *(uint32_t*)(thread + 0x100) = ByteSwap(g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE)); // teb pointer
    *(thread + 0x10C) = cpuNumber;

    *(uint32_t*)(thread + PCR_SIZE + 0x10) = 0xFFFFFFFF; // that one TLS entry that felt quirky
    *(uint32_t*)(thread + PCR_SIZE + TLS_SIZE + 0x14C) = ByteSwap(GuestThread::GetCurrentThreadId()); // thread id

    ppcContext.r1.u64 = g_memory.MapVirtual(thread + PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE); // stack pointer
    ppcContext.r13.u64 = g_memory.MapVirtual(thread);
    ppcContext.fpscr.loadFromHost();

    assert(GetPPCContext() == nullptr);
    SetPPCContext(ppcContext);
    report::SetThreadGuestContext(&ppcContext);  // for crash and hang reports
}

GuestThreadContext::~GuestThreadContext()
{
    report::SetThreadGuestContext(nullptr);
    g_userHeap.Free(thread);
}

#ifdef USE_PTHREAD
static size_t GetStackSize()
{
    // Cache as this should not change.
    static size_t stackSize = 0;
    if (stackSize == 0)
    {
        // 8 MiB is a typical default.
        constexpr auto defaultSize = 8 * 1024 * 1024;
        struct rlimit lim;
        const auto ret = getrlimit(RLIMIT_STACK, &lim);
#if defined(__APPLE__) && TARGET_OS_IOS
        // iOS limits only the main thread's stack (1 MiB); a thread made
        // here may have 8 like everywhere else.
        if (false)
#else
        if (ret == 0 && lim.rlim_cur < defaultSize)
#endif
        {
            // Use what the system allows.
            stackSize = lim.rlim_cur;
        }
        else
        {
            stackSize = defaultSize;
        }
    }
    return stackSize;
}

void SetHostThreadName(const char* name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
    report::RegisterThread(name);  // hang reports sample named threads
}

static void* GuestThreadFunc(void* arg)
{
    GuestThreadHandle* hThread = (GuestThreadHandle*)arg;
#else
static void GuestThreadFunc(GuestThreadHandle* hThread)
{
#endif
    t_threadId = hThread->id;
    t_self = hThread;
    {
        char name[16];
        snprintf(name, sizeof(name), "g%u-%08X", hThread->id, hThread->params.function);
        SetHostThreadName(name);
    }
    hThread->suspended.wait(true);
    GuestThread::Start(hThread->params);
    // SpeedBreaker: signal waiters, then drop the thread's own reference.
    {
        std::lock_guard lock(dispatcher::Lock());
        hThread->finished = true;
        dispatcher::NotifyAllLocked();
    }
    ReleaseKernelObject(hThread);
#ifdef USE_PTHREAD
    return nullptr;
#endif
}

GuestThreadHandle::GuestThreadHandle(const GuestThreadParams& params)
    : params(params), id(s_nextThreadId++), suspended((params.flags & 0x1) != 0)
#ifdef USE_PTHREAD
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, GetStackSize());
    refs = 2;  // SpeedBreaker: the handle's reference + the thread's own
    const auto ret = pthread_create(&thread, &attr, GuestThreadFunc, this);
    if (ret != 0) {
        fprintf(stderr, "pthread_create failed with error code 0x%X.\n", ret);
        return;
    }
    pthread_detach(thread);
}
#else
      , thread(GuestThreadFunc, this)
{
}
#endif

// SpeedBreaker: threads are detached at creation; closing the last handle
// must never block on (or kill) a running thread.
GuestThreadHandle::~GuestThreadHandle()
{
}


uint32_t GuestThreadHandle::GetThreadId() const
{
    return id;
}


uint32_t GuestThread::Start(const GuestThreadParams& params)
{
    const auto procMask = (uint8_t)(params.flags >> 24);
    const auto cpuNumber = procMask == 0 ? 0 : 7 - std::countl_zero(procMask);

    GuestThreadContext ctx(cpuNumber);
    ctx.ppcContext.r3.u64 = params.value;
    ctx.ppcContext.r4.u64 = params.value2;

    try
    {
        g_memory.FindFunction(params.function)(ctx.ppcContext, g_memory.base);
    }
    catch (const GuestThreadExit& exit)
    {
        ctx.ppcContext.r3.u64 = exit.exitCode;
    }

    return ctx.ppcContext.r3.u32;
}

GuestThreadHandle* GuestThread::Start(const GuestThreadParams& params, uint32_t* threadId)
{
    auto hThread = CreateKernelObject<GuestThreadHandle>(params);

    if (threadId != nullptr)
    {
        *threadId = hThread->GetThreadId();
    }

    return hThread;
}

uint32_t GuestThread::GetCurrentThreadId()
{
    return CurrentThreadId();
}

void GuestThread::SetLastError(uint32_t error)
{
    auto* thread = (char*)g_memory.Translate(GetPPCContext()->r13.u32);
    if (*(uint32_t*)(thread + 0x150))
    {
        // Program doesn't want errors
        return;
    }

    // TEB + 0x160 : Win32LastError
    *(uint32_t*)(thread + TEB_OFFSET + 0x160) = ByteSwap(error);
}

#ifdef _WIN32
void GuestThread::SetThreadName(uint32_t threadId, const char* name)
{
#pragma pack(push,8)
    const DWORD MS_VC_EXCEPTION = 0x406D1388;

    typedef struct tagTHREADNAME_INFO
    {
        DWORD dwType; // Must be 0x1000.
        LPCSTR szName; // Pointer to name (in user addr space).
        DWORD dwThreadID; // Thread ID (-1=caller thread).
        DWORD dwFlags; // Reserved for future use, must be zero.
    } THREADNAME_INFO;
#pragma pack(pop)

    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = name;
    info.dwThreadID = threadId;
    info.dwFlags = 0;

    __try
    {
        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
#endif
