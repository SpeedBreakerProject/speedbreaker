// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Synchronisation exports on top of kernel/dispatcher. Signatures and
// return values follow Xenia's xboxkrnl_threading.cc.
#include <stdafx.h>
#include "dispatcher.h"
#include "function.h"
#include <cpu/guest_thread.h>
#include <cpu/guest_time.h>
#include <cpu/host_cpu.h>

using namespace dispatcher;

namespace
{
    constexpr uint32_t STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;

    template<typename T>
    T* FromHandle(uint32_t handle)
    {
        if (handle == GUEST_INVALID_HANDLE_VALUE || !IsKernelObject(handle))
            return nullptr;
        return dynamic_cast<T*>(GetKernelObject(handle));
    }

    // Named objects (OBJECT_ATTRIBUTES with a name) aren't supported yet;
    // make their first use visible instead of silently creating a new one.
    void CheckUnnamed(const XOBJECT_ATTRIBUTES* attributes, const char* who)
    {
        if (attributes != nullptr && attributes->Name.get() != 0)
            fprintf(stderr, "[sync] %s: named objects are not supported yet (creating an unnamed one)\n", who);
    }

    uint32_t WaitHandles(std::span<const be<uint32_t>> handles, bool waitAll, const be<int64_t>* timeout)
    {
        std::vector<Waitable*> objects;
        for (uint32_t h : handles)
        {
            Waitable* w = WaitableFromHandle(h);
            if (w == nullptr)
                return STATUS_INVALID_HANDLE;
            objects.push_back(w);
        }
        return WaitForObjects(objects, waitAll, Deadline(timeout));
    }
}

// ---- Events ------------------------------------------------------------------

uint32_t NtCreateEvent(be<uint32_t>* handle, XOBJECT_ATTRIBUTES* attributes, uint32_t eventType, uint32_t initialState)
{
    CheckUnnamed(attributes, "NtCreateEvent");
    // eventType: 0 = NotificationEvent (manual reset), 1 = SynchronizationEvent.
    *handle = GetKernelHandle(CreateKernelObject<EventObject>(eventType == 0, initialState != 0));
    return STATUS_SUCCESS;
}

uint32_t KeSetEvent(XKEVENT* event, uint32_t increment, uint32_t wait)
{
    return static_cast<EventObject*>(BindDispatcherHeader(*event))->Set();
}

uint32_t KeResetEvent(XKEVENT* event)
{
    return static_cast<EventObject*>(BindDispatcherHeader(*event))->Reset();
}

uint32_t KePulseEvent(XKEVENT* event, uint32_t increment, uint32_t wait)
{
    return static_cast<EventObject*>(BindDispatcherHeader(*event))->Pulse();
}

uint32_t NtSetEvent(uint32_t handle, be<uint32_t>* previousState)
{
    auto* e = FromHandle<EventObject>(handle);
    if (e == nullptr)
        return STATUS_INVALID_HANDLE;
    bool previous = e->Set();
    if (previousState)
        *previousState = previous;
    return STATUS_SUCCESS;
}

uint32_t NtClearEvent(uint32_t handle)
{
    auto* e = FromHandle<EventObject>(handle);
    if (e == nullptr)
        return STATUS_INVALID_HANDLE;
    e->Reset();
    return STATUS_SUCCESS;
}

uint32_t NtPulseEvent(uint32_t handle, be<uint32_t>* previousState)
{
    auto* e = FromHandle<EventObject>(handle);
    if (e == nullptr)
        return STATUS_INVALID_HANDLE;
    bool previous = e->Pulse();
    if (previousState)
        *previousState = previous;
    return STATUS_SUCCESS;
}

// ---- Semaphores ----------------------------------------------------------------

void KeInitializeSemaphore(XKSEMAPHORE* semaphore, uint32_t count, uint32_t limit)
{
    // Guest-embedded: initialise the header; the host object binds on first use.
    semaphore->Header.Type = 5;
    semaphore->Header.SignalState = count;
    semaphore->Header.WaitListHead.Flink = 0;
    semaphore->Header.WaitListHead.Blink = 0;
    semaphore->Limit = limit;
}

uint32_t KeReleaseSemaphore(XKSEMAPHORE* semaphore, uint32_t increment, uint32_t adjustment, uint32_t wait)
{
    int32_t previous = 0;
    auto* s = static_cast<SemaphoreObject*>(BindDispatcherHeader(semaphore->Header));
    uint32_t status = s->Release(int32_t(adjustment), &previous);
    assert(status == STATUS_SUCCESS && "KeReleaseSemaphore past its limit");
    return uint32_t(previous);
}

uint32_t NtCreateSemaphore(be<uint32_t>* handle, XOBJECT_ATTRIBUTES* attributes, uint32_t count, uint32_t limit)
{
    CheckUnnamed(attributes, "NtCreateSemaphore");
    *handle = GetKernelHandle(CreateKernelObject<SemaphoreObject>(int32_t(count), int32_t(limit)));
    return STATUS_SUCCESS;
}

uint32_t NtReleaseSemaphore(uint32_t handle, uint32_t releaseCount, be<uint32_t>* previousCount)
{
    auto* s = FromHandle<SemaphoreObject>(handle);
    if (s == nullptr)
        return STATUS_INVALID_HANDLE;
    int32_t previous = 0;
    uint32_t status = s->Release(int32_t(releaseCount), &previous);
    if (previousCount)
        *previousCount = uint32_t(previous);
    return status;
}

// ---- Mutants -------------------------------------------------------------------

uint32_t NtCreateMutant(be<uint32_t>* handle, XOBJECT_ATTRIBUTES* attributes, uint32_t initialOwner)
{
    CheckUnnamed(attributes, "NtCreateMutant");
    *handle = GetKernelHandle(CreateKernelObject<MutantObject>(initialOwner != 0));
    return STATUS_SUCCESS;
}

uint32_t NtReleaseMutant(uint32_t handle, be<uint32_t>* previousCount)
{
    auto* m = FromHandle<MutantObject>(handle);
    if (m == nullptr)
        return STATUS_INVALID_HANDLE;
    int32_t previous = 0;
    uint32_t status = m->Release(&previous);
    // Xenia notes the second argument is unused by titles it has seen.
    return status;
}

// ---- Timers --------------------------------------------------------------------

uint32_t NtCreateTimer(be<uint32_t>* handle, XOBJECT_ATTRIBUTES* attributes, uint32_t timerType)
{
    CheckUnnamed(attributes, "NtCreateTimer");
    *handle = GetKernelHandle(CreateKernelObject<TimerObject>(timerType));
    return STATUS_SUCCESS;
}

uint32_t NtSetTimerEx(uint32_t handle, be<int64_t>* dueTime, uint32_t apcRoutine, uint32_t apcMode,
    uint32_t apcContext, uint32_t resume, uint32_t periodMs, be<uint32_t>* previousState)
{
    auto* t = FromHandle<TimerObject>(handle);
    if (t == nullptr)
        return STATUS_INVALID_HANDLE;
    if (apcRoutine != 0)
    {
        // Timer APCs need an APC queue on the target thread; make the first
        // use loud rather than never calling the routine.
        fprintf(stderr, "[sync] NtSetTimerEx with an APC routine (%08X) is not supported yet\n", apcRoutine);
        assert(false && "timer APC routines are not implemented");
    }
    bool previous = t->Set(Deadline(dueTime), periodMs);
    if (previousState)
        *previousState = previous;
    return STATUS_SUCCESS;
}

uint32_t NtCancelTimer(uint32_t handle, be<uint32_t>* currentState)
{
    auto* t = FromHandle<TimerObject>(handle);
    if (t == nullptr)
        return STATUS_INVALID_HANDLE;
    bool state = t->Cancel();
    if (currentState)
        *currentState = state;
    return STATUS_SUCCESS;
}

// ---- Waits ---------------------------------------------------------------------

uint32_t KeWaitForSingleObject(XDISPATCHER_HEADER* object, uint32_t waitReason, uint32_t waitMode,
    uint32_t alertable, be<int64_t>* timeout)
{
    Waitable* w = BindDispatcherHeader(*object);
    if (w == nullptr)
        return STATUS_INVALID_PARAMETER;
    return WaitForObjects({ &w, 1 }, false, Deadline(timeout));
}

uint32_t KeWaitForMultipleObjects(uint32_t count, xpointer<XDISPATCHER_HEADER>* objects, uint32_t waitType,
    uint32_t waitReason, uint32_t waitMode, uint32_t alertable, be<int64_t>* timeout, void* waitBlocks)
{
    std::vector<Waitable*> list;
    for (uint32_t i = 0; i < count; i++)
    {
        Waitable* w = BindDispatcherHeader(*objects[i]);
        if (w == nullptr)
            return STATUS_INVALID_PARAMETER;
        list.push_back(w);
    }
    // waitType: 0 = WaitAll, 1 = WaitAny.
    return WaitForObjects(list, waitType == 0, Deadline(timeout));
}

uint32_t NtWaitForSingleObjectEx(uint32_t handle, uint32_t waitMode, uint32_t alertable, be<int64_t>* timeout)
{
    be<uint32_t> h = handle;
    return WaitHandles({ &h, 1 }, false, timeout);
}

uint32_t NtWaitForMultipleObjectsEx(uint32_t count, be<uint32_t>* handles, uint32_t waitType, uint32_t waitMode,
    uint32_t alertable, be<int64_t>* timeout)
{
    return WaitHandles({ handles, count }, waitType == 0, timeout);
}

uint32_t NtSignalAndWaitForSingleObjectEx(uint32_t signalHandle, uint32_t waitHandle, uint32_t alertable,
    uint32_t unknown, be<int64_t>* timeout)
{
    KernelObject* signal = IsKernelObject(signalHandle) ? GetKernelObject(signalHandle) : nullptr;
    if (auto* e = dynamic_cast<EventObject*>(signal))
        e->Set();
    else if (auto* s = dynamic_cast<SemaphoreObject*>(signal))
        s->Release(1, nullptr);
    else if (auto* m = dynamic_cast<MutantObject*>(signal))
        m->Release(nullptr);
    else
        return STATUS_OBJECT_TYPE_MISMATCH;
    be<uint32_t> h = waitHandle;
    return WaitHandles({ &h, 1 }, false, timeout);
}

// ---- Delays --------------------------------------------------------------------

uint32_t KeDelayExecutionThread(uint32_t waitMode, uint32_t alertable, be<int64_t>* interval)
{
    auto deadline = Deadline(interval);
    if (!deadline || *deadline <= guesttime::NowNs())
    {
        // While the game is suspended a polling loop would spin through it
        // (no guest time passes, so what it polls for can't come): park.
        guesttime::WaitWhileSuspended();

        // Sleep(0) polling loops: one game thread (g2) spent ~90% of a core
        // in them, 60% of it in sched_yield, and kept one of the Steam
        // Machine's two fast cores (see hostcpu::ReserveFastCore). After 1 ms
        // of back-to-back zero delays each one sleeps 100 us instead (a new
        // poll starts exact). Linux only: macOS coalesces default-QoS timers
        // to ~10 ms. NFSMW_SLEEP0_BACKOFF=0 always yields.
#ifdef __linux__
        static const bool backOff = [] { const char* v = std::getenv("NFSMW_SLEEP0_BACKOFF"); return !v || v[0] != '0'; }();
        thread_local Clock::time_point firstZero{}, lastZero{};
        auto now = Clock::now();
        if (now - lastZero > std::chrono::microseconds(300))
            firstZero = now;
        lastZero = now;
        if (backOff && now - firstZero > std::chrono::milliseconds(1))
        {
            hostcpu::TightenTimerSlack();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            lastZero = Clock::now();
            return STATUS_SUCCESS;
        }
#endif
        std::this_thread::yield();
    }
    else
        guesttime::SleepUntil(*deadline);  // longer by any suspension
    return STATUS_SUCCESS;
}

uint32_t NtYieldExecution()
{
    guesttime::WaitWhileSuspended();  // as KeDelayExecutionThread's polls
    std::this_thread::yield();
    return STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__NtCreateEvent, NtCreateEvent);
GUEST_FUNCTION_HOOK(__imp__KeSetEvent, KeSetEvent);
GUEST_FUNCTION_HOOK(__imp__KeResetEvent, KeResetEvent);
GUEST_FUNCTION_HOOK(__imp__KePulseEvent, KePulseEvent);
GUEST_FUNCTION_HOOK(__imp__NtSetEvent, NtSetEvent);
GUEST_FUNCTION_HOOK(__imp__NtClearEvent, NtClearEvent);
GUEST_FUNCTION_HOOK(__imp__NtPulseEvent, NtPulseEvent);
GUEST_FUNCTION_HOOK(__imp__KeInitializeSemaphore, KeInitializeSemaphore);
GUEST_FUNCTION_HOOK(__imp__KeReleaseSemaphore, KeReleaseSemaphore);
GUEST_FUNCTION_HOOK(__imp__NtCreateSemaphore, NtCreateSemaphore);
GUEST_FUNCTION_HOOK(__imp__NtReleaseSemaphore, NtReleaseSemaphore);
GUEST_FUNCTION_HOOK(__imp__NtCreateMutant, NtCreateMutant);
GUEST_FUNCTION_HOOK(__imp__NtReleaseMutant, NtReleaseMutant);
GUEST_FUNCTION_HOOK(__imp__NtCreateTimer, NtCreateTimer);
GUEST_FUNCTION_HOOK(__imp__NtSetTimerEx, NtSetTimerEx);
GUEST_FUNCTION_HOOK(__imp__NtCancelTimer, NtCancelTimer);
GUEST_FUNCTION_HOOK(__imp__KeWaitForSingleObject, KeWaitForSingleObject);
GUEST_FUNCTION_HOOK(__imp__KeWaitForMultipleObjects, KeWaitForMultipleObjects);
GUEST_FUNCTION_HOOK(__imp__NtWaitForSingleObjectEx, NtWaitForSingleObjectEx);
GUEST_FUNCTION_HOOK(__imp__NtWaitForMultipleObjectsEx, NtWaitForMultipleObjectsEx);
GUEST_FUNCTION_HOOK(__imp__NtSignalAndWaitForSingleObjectEx, NtSignalAndWaitForSingleObjectEx);
GUEST_FUNCTION_HOOK(__imp__KeDelayExecutionThread, KeDelayExecutionThread);
GUEST_FUNCTION_HOOK(__imp__NtYieldExecution, NtYieldExecution);

// Critical regions disable normal kernel APC delivery. We don't deliver
// APCs yet (their first use asserts), so there is nothing to disable.
void KeEnterCriticalRegion() {}
void KeLeaveCriticalRegion() {}

GUEST_FUNCTION_HOOK(__imp__KeEnterCriticalRegion, KeEnterCriticalRegion);
GUEST_FUNCTION_HOOK(__imp__KeLeaveCriticalRegion, KeLeaveCriticalRegion);

// Ends the calling thread: unwinds to GuestThread::Start, which then marks the
// thread finished (waking waiters) and exits it.
void ExTerminateThread(uint32_t exitStatus)
{
    throw GuestThreadExit{ exitStatus };
}

GUEST_FUNCTION_HOOK(__imp__ExTerminateThread, ExTerminateThread);
