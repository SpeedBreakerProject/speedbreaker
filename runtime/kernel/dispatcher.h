// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Kernel dispatcher: every waitable object (event, semaphore, mutant, timer,
// thread) exposes "is it signaled?" and "consume the signal", and one wait
// routine serves single and multiple waits, wait-any and wait-all, with
// relative, absolute or infinite guest timeouts. Semantics follow Xenia's
// XObject/XEvent/XSemaphore/XMutant/XTimer.
//
// All state changes and checks happen under one global lock; waiters sleep on
// one condition variable and re-check after every state change. Simple and
// correct first; revisit only if profiling shows contention.
//
// Timeouts and timers run on guest time (guesttime::NowNs()), which stands
// still while the game is suspended: a timed wait or a timer that would come
// due then parks without a timeout instead, and WakeAfterResume wakes it.
#pragma once
#include <chrono>
#include <optional>
#include <span>

#include "xdm.h"

namespace dispatcher
{
    using Clock = std::chrono::steady_clock;

    std::mutex& Lock();
    // Wake every waiter to re-check. Call with Lock() held, after a change.
    void NotifyAllLocked();

    // Guest time in 100 ns units since 1601 (KeQuerySystemTime's clock).
    int64_t GuestSystemTime();
    // Deadlines are guest ns (guesttime::NowNs()). nullptr = infinite;
    // negative = relative 100 ns; positive = absolute guest system time;
    // 0 = poll.
    std::optional<int64_t> Deadline(const be<int64_t>* timeout);
    std::optional<int64_t> DeadlineFromMilliseconds(uint32_t ms);

    // guesttime's resume hook, on its own thread: wakes the waits and the
    // timer thread that parked without a timeout while the game was
    // suspended, so they measure guest time again.
    void WakeAfterResume();
    // Kernel timer firings so far (the suspension log counts them).
    uint64_t TimerFirings();
}

struct Waitable : KernelObject
{
    // Number of threads currently waiting on this object (for pulses).
    uint32_t waiters = 0;

    // Both are called with dispatcher::Lock() held, on the waiting thread.
    virtual bool IsSignaledLocked() const = 0;
    virtual void AcquireLocked() {}

    // KernelObject interface: millisecond timeout (INFINITE = forever).
    uint32_t Wait(uint32_t timeoutMs) override;
};

// Returns STATUS_WAIT_0 + index (wait-any), STATUS_WAIT_0 (wait-all) or
// STATUS_TIMEOUT. `deadline` is guest ns (dispatcher::Deadline).
uint32_t WaitForObjects(std::span<Waitable* const> objects, bool waitAll,
    std::optional<int64_t> deadline);

struct EventObject final : Waitable, HostObject<XKEVENT>
{
    bool manualReset;
    bool signaled;
    uint32_t pulseWakes = 0;   // waiters still to be released by a pulse
    XKEVENT* guest = nullptr;  // guest-embedded event: SignalState is mirrored

    explicit EventObject(XKEVENT* header);
    EventObject(bool manualReset, bool initialState);

    bool IsSignaledLocked() const override { return signaled; }
    void AcquireLocked() override;

    // Each returns the previous signal state.
    bool Set();
    bool Reset();
    bool Pulse();

private:
    void MirrorLocked();
};

struct SemaphoreObject final : Waitable, HostObject<XKSEMAPHORE>
{
    int32_t count;
    int32_t limit;
    XKSEMAPHORE* guest = nullptr;

    explicit SemaphoreObject(XKSEMAPHORE* semaphore);
    SemaphoreObject(int32_t count, int32_t limit);

    bool IsSignaledLocked() const override { return count > 0; }
    void AcquireLocked() override;
    // Returns STATUS_SUCCESS or STATUS_SEMAPHORE_LIMIT_EXCEEDED.
    uint32_t Release(int32_t releaseCount, int32_t* previousCount);
};

struct MutantObject final : Waitable
{
    uint32_t owner = 0;      // guest thread id; 0 = unowned
    uint32_t recursion = 0;

    explicit MutantObject(bool initialOwner);

    bool IsSignaledLocked() const override;
    void AcquireLocked() override;
    // Returns STATUS_SUCCESS or STATUS_MUTANT_NOT_OWNED; *previous = old count.
    uint32_t Release(int32_t* previous);
};

struct TimerObject final : Waitable
{
    bool manualReset;        // NotificationTimer (type 0) vs SynchronizationTimer (1)
    bool signaled = false;
    uint64_t generation = 0; // bumped by Set/Cancel; stale firings are ignored
    uint32_t periodMs = 0;
    int64_t due = 0;         // guest ns

    explicit TimerObject(uint32_t timerType);

    bool IsSignaledLocked() const override { return signaled; }
    void AcquireLocked() override { if (!manualReset) signaled = false; }

    // Returns the previous signal state. `dueTime` is guest ns (nullopt: now).
    bool Set(std::optional<int64_t> dueTime, uint32_t periodMs);
    bool Cancel();
};

// Host object bound to a guest-embedded dispatcher header (KEVENT in game
// memory, ...), created on first use from the header's Type.
Waitable* BindDispatcherHeader(XDISPATCHER_HEADER& header);

// Waitable for a handle, including the current-thread pseudo-handle.
// Returns nullptr for invalid or non-waitable handles.
Waitable* WaitableFromHandle(uint32_t handle);
