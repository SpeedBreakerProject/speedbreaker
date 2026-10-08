// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include "dispatcher.h"

#include <condition_variable>
#include <queue>

#include <cpu/guest_thread.h>
#include <cpu/guest_time.h>

namespace dispatcher
{
    static std::mutex s_lock;
    static std::condition_variable s_cv;

    std::mutex& Lock() { return s_lock; }
    void NotifyAllLocked() { s_cv.notify_all(); }

    int64_t GuestSystemTime()
    {
        return guesttime::SystemTime100ns();
    }

    std::optional<int64_t> Deadline(const be<int64_t>* timeout)
    {
        if (timeout == nullptr)
            return std::nullopt;
        int64_t t = *timeout;
        // INT64_MIN is the game's Sleep(INFINITE) (its SleepEx, from a file
        // read wrapper): -t * 100 used to wrap to 0, a wait that returns at
        // once, and the game runs that way; keep it so.
        int64_t relative100ns = t <= 0 ? (t == INT64_MIN ? 0 : -t) : std::max<int64_t>(0, t - GuestSystemTime());
        // 73 years is forever, and keeps guest ns (and the host deadline a
        // wait derives from it) from overflowing.
        relative100ns = std::min<int64_t>(relative100ns, INT64_MAX / 400);
        return guesttime::NowNs() + relative100ns * 100;
    }

    std::optional<int64_t> DeadlineFromMilliseconds(uint32_t ms)
    {
        if (ms == INFINITE)
            return std::nullopt;
        return guesttime::NowNs() + int64_t(ms) * 1'000'000;
    }

    std::condition_variable& ConditionVariable() { return s_cv; }
}

using namespace dispatcher;

uint32_t WaitForObjects(std::span<Waitable* const> objects, bool waitAll, std::optional<int64_t> deadline)
{
    std::unique_lock lock(Lock());
    for (Waitable* o : objects)
        o->waiters++;
    struct Leave
    {
        std::span<Waitable* const> objects;
        ~Leave() { for (Waitable* o : objects) o->waiters--; }
    } leave{ objects };

    while (true)
    {
        if (waitAll)
        {
            bool all = true;
            for (Waitable* o : objects)
                all &= o->IsSignaledLocked();
            if (all)
            {
                for (Waitable* o : objects)
                    o->AcquireLocked();
                return STATUS_WAIT_0;
            }
        }
        else
        {
            for (size_t i = 0; i < objects.size(); i++)
            {
                if (objects[i]->IsSignaledLocked())
                {
                    objects[i]->AcquireLocked();
                    return STATUS_WAIT_0 + uint32_t(i);
                }
            }
        }

        if (!deadline)
        {
            ConditionVariable().wait(lock);
            continue;
        }
        // Guest time stands still while the game is suspended, so no timeout
        // can run out: wait without one until a change or the resume. That
        // includes a poll (timeout 0), which would otherwise return at once
        // to a guest loop that polls again, spinning a core throughout.
        // WakeAfterResume notifies under Lock() once Suspended() is false,
        // so this thread is either waiting by then or reads false here.
        if (guesttime::Suspended())
        {
            ConditionVariable().wait(lock);
            continue;
        }
        int64_t left = *deadline - guesttime::NowNs();
        if (left <= 0)
            return STATUS_TIMEOUT;
        // A suspension that starts meanwhile is seen when this wakes at the
        // host deadline, with guest time still short of it.
        ConditionVariable().wait_until(lock, Clock::now() + std::chrono::nanoseconds(left));
    }
}

uint32_t Waitable::Wait(uint32_t timeoutMs)
{
    Waitable* self = this;
    return WaitForObjects({ &self, 1 }, false, DeadlineFromMilliseconds(timeoutMs));
}

// ---- Event -----------------------------------------------------------------

EventObject::EventObject(XKEVENT* header)
    : manualReset(header->Type == 0), signaled(header->SignalState != 0), guest(header)
{
}

EventObject::EventObject(bool manualReset, bool initialState)
    : manualReset(manualReset), signaled(initialState)
{
}

void EventObject::MirrorLocked()
{
    if (guest != nullptr)
        guest->SignalState = signaled ? 1 : 0;
}

void EventObject::AcquireLocked()
{
    if (pulseWakes > 0 && --pulseWakes == 0)
        signaled = false;
    else if (!manualReset)
        signaled = false;
    MirrorLocked();
}

bool EventObject::Set()
{
    std::lock_guard lock(Lock());
    bool previous = signaled;
    signaled = true;
    pulseWakes = 0;
    MirrorLocked();
    NotifyAllLocked();
    return previous;
}

bool EventObject::Reset()
{
    std::lock_guard lock(Lock());
    bool previous = signaled;
    signaled = false;
    pulseWakes = 0;
    MirrorLocked();
    return previous;
}

// Releases the threads waiting right now (all of them for a notification
// event, one for a synchronization event), then leaves the event reset.
bool EventObject::Pulse()
{
    std::lock_guard lock(Lock());
    bool previous = signaled;
    if (waiters == 0)
    {
        signaled = false;
    }
    else
    {
        signaled = true;
        pulseWakes = manualReset ? waiters : 1;
        NotifyAllLocked();
    }
    MirrorLocked();
    return previous;
}

// ---- Semaphore -------------------------------------------------------------

SemaphoreObject::SemaphoreObject(XKSEMAPHORE* semaphore)
    : count(int32_t(uint32_t(semaphore->Header.SignalState))), limit(int32_t(uint32_t(semaphore->Limit))), guest(semaphore)
{
}

SemaphoreObject::SemaphoreObject(int32_t count, int32_t limit)
    : count(count), limit(limit)
{
}

void SemaphoreObject::AcquireLocked()
{
    count--;
    if (guest != nullptr)
        guest->Header.SignalState = uint32_t(count);
}

uint32_t SemaphoreObject::Release(int32_t releaseCount, int32_t* previousCount)
{
    std::lock_guard lock(Lock());
    if (previousCount)
        *previousCount = count;
    if (releaseCount <= 0 || int64_t(count) + releaseCount > limit)
        return 0xC0000047; // STATUS_SEMAPHORE_LIMIT_EXCEEDED
    count += releaseCount;
    if (guest != nullptr)
        guest->Header.SignalState = uint32_t(count);
    NotifyAllLocked();
    return STATUS_SUCCESS;
}

// ---- Mutant ----------------------------------------------------------------

MutantObject::MutantObject(bool initialOwner)
{
    if (initialOwner)
    {
        owner = GuestThread::GetCurrentThreadId();
        recursion = 1;
    }
}

bool MutantObject::IsSignaledLocked() const
{
    return recursion == 0 || owner == GuestThread::GetCurrentThreadId();
}

void MutantObject::AcquireLocked()
{
    owner = GuestThread::GetCurrentThreadId();
    recursion++;
}

uint32_t MutantObject::Release(int32_t* previous)
{
    std::lock_guard lock(Lock());
    if (previous)
        *previous = int32_t(recursion);
    if (recursion == 0 || owner != GuestThread::GetCurrentThreadId())
        return 0xC0000046; // STATUS_MUTANT_NOT_OWNED
    if (--recursion == 0)
    {
        owner = 0;
        NotifyAllLocked();
    }
    return STATUS_SUCCESS;
}

// ---- Timer -----------------------------------------------------------------

namespace
{
    std::atomic<uint64_t> s_firings{ 0 };

    struct TimerEntry
    {
        int64_t due;  // guest ns
        TimerObject* timer;
        uint64_t generation;
        bool operator>(const TimerEntry& o) const { return due > o.due; }
    };

    // One host thread fires every timer. Queued entries hold a reference on
    // their timer so a closed handle can't leave a dangling pointer.
    struct TimerThread
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<>> queue;
        std::thread thread{ [this] { Run(); } };

        void Schedule(TimerObject* timer, int64_t due, uint64_t generation)
        {
            RetainKernelObject(timer);
            std::lock_guard lock(mutex);
            queue.push({ due, timer, generation });
            cv.notify_one();
        }

        void Run()
        {
            std::unique_lock lock(mutex);
            while (true)
            {
                // No timer comes due while the game is suspended (guest time
                // stands still). Suspended() is read under `mutex`, which
                // WakeAfterResume takes to notify once it's false.
                if (queue.empty() || guesttime::Suspended())
                {
                    cv.wait(lock);
                    continue;
                }
                TimerEntry next = queue.top();
                int64_t left = next.due - guesttime::NowNs();
                if (left > 0)
                {
                    cv.wait_until(lock, Clock::now() + std::chrono::nanoseconds(left));
                    continue;
                }
                queue.pop();
                lock.unlock();
                Fire(next);
                ReleaseKernelObject(next.timer);
                lock.lock();
            }
        }

        void Fire(const TimerEntry& e)
        {
            std::lock_guard lock(Lock());
            TimerObject* t = e.timer;
            if (t->generation != e.generation)
                return;  // cancelled or re-armed since
            t->signaled = true;
            s_firings.fetch_add(1, std::memory_order_relaxed);
            NotifyAllLocked();
            if (t->periodMs != 0)
            {
                // From the last due time in guest time: a suspension adds no
                // periods to catch up on.
                t->due = e.due + int64_t(t->periodMs) * 1'000'000;
                RetainKernelObject(t);
                std::lock_guard qlock(mutex);
                queue.push({ t->due, t, t->generation });
            }
        }
    };

    // Created by the first timer armed. WakeAfterResume reads it under
    // s_timersMutex too, so it either finds the thread or the thread starts
    // after the resume and reads Suspended() false itself.
    std::mutex s_timersMutex;
    TimerThread* s_timers = nullptr;

    TimerThread& Timers()
    {
        std::lock_guard lock(s_timersMutex);
        if (s_timers == nullptr)
        {
            s_timers = new TimerThread();
            s_timers->thread.detach();
        }
        return *s_timers;
    }
}

void dispatcher::WakeAfterResume()
{
    // Two locks one after the other, never nested (Fire takes Lock() and
    // then the timer mutex). Both are released by their waiters' cv waits.
    {
        std::lock_guard lock(Lock());
        NotifyAllLocked();
    }
    TimerThread* timers;
    {
        std::lock_guard lock(s_timersMutex);
        timers = s_timers;  // never created here
    }
    if (timers != nullptr)
    {
        std::lock_guard lock(timers->mutex);
        timers->cv.notify_all();
    }
}

uint64_t dispatcher::TimerFirings()
{
    return s_firings.load(std::memory_order_relaxed);
}

TimerObject::TimerObject(uint32_t timerType)
    : manualReset(timerType == 0)
{
}

bool TimerObject::Set(std::optional<int64_t> dueTime, uint32_t period)
{
    uint64_t gen;
    int64_t when;
    bool previous;
    {
        std::lock_guard lock(Lock());
        previous = signaled;
        signaled = false;
        periodMs = period;
        due = dueTime.value_or(guesttime::NowNs());
        gen = ++generation;
        when = due;
    }
    Timers().Schedule(this, when, gen);
    return previous;
}

bool TimerObject::Cancel()
{
    std::lock_guard lock(Lock());
    bool previous = signaled;
    generation++;
    return previous;
}

// ---- Binding -----------------------------------------------------------------

Waitable* BindDispatcherHeader(XDISPATCHER_HEADER& header)
{
    std::lock_guard guard{ g_kernelLock };
    if (header.WaitListHead.Flink == OBJECT_SIGNATURE)
        return static_cast<Waitable*>(g_memory.Translate(header.WaitListHead.Blink.get()));

    Waitable* obj;
    switch (header.Type)
    {
    case 0: // NotificationEvent
    case 1: // SynchronizationEvent
        obj = CreateKernelObject<EventObject>(reinterpret_cast<XKEVENT*>(&header));
        break;
    case 5: // Semaphore
        obj = CreateKernelObject<SemaphoreObject>(reinterpret_cast<XKSEMAPHORE*>(&header));
        break;
    default:
        fprintf(stderr, "[dispatcher] guest dispatcher header type %u is not supported yet\n", header.Type);
        assert(false && "unsupported guest dispatcher object type");
        return nullptr;
    }
    header.WaitListHead.Flink = OBJECT_SIGNATURE;
    header.WaitListHead.Blink = g_memory.MapVirtual(obj);
    return obj;
}

Waitable* WaitableFromHandle(uint32_t handle)
{
    KernelObject* obj;
    if (handle == 0xFFFFFFFE)
        obj = GuestThread::GetCurrentThreadObject();
    else if (handle != GUEST_INVALID_HANDLE_VALUE && IsKernelObject(handle))
        obj = GetKernelObject(handle);
    else
        return nullptr;
    return dynamic_cast<Waitable*>(obj);
}
