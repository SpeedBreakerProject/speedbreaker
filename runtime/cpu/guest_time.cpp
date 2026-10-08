// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See guest_time.h.
// Standard headers only, so a test can build it alone.
#include "guest_time.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace guesttime
{
    State g_state;

    namespace
    {
        std::mutex s_mutex;  // a leaf: nothing else is taken while it's held
        // Never destroyed: threads stay blocked on them for good (the hook
        // thread, a waiter parked in a suspension), and glibc's
        // pthread_cond_destroy waits for blocked waiters, so an exit through
        // main's return would hang (as video/presenter.cpp's timing waiter did).
        std::condition_variable& s_cv = *new std::condition_variable;
        std::atomic<uint64_t> s_epoch{ 0 };
        std::atomic<int64_t> s_lastResume{ 0 };

        // The resume hook's thread: Resume() only bumps s_resumes and
        // notifies; this thread then calls the hook, which takes the
        // kernel's locks (held by game threads, possibly across a write
        // fault waiting for the GPU) where UIKit's callback mustn't wait.
        void (*s_hook)() = nullptr;
        uint64_t s_resumes = 0, s_hooked = 0;  // under s_mutex
        std::condition_variable& s_hookCv = *new std::condition_variable;

        // A reader's counter read isn't ordered with the loads around it, so
        // it can land a little after the writer's own: each transition moves
        // guest time on by this much, so it never runs backwards (game code
        // subtracts mftb and the tick count unsigned).
        constexpr int64_t kMarginTicks = 50, kMarginNs = 1000, kMargin100ns = 10;

        template<class F>
        void Write(F change)
        {
            uint64_t seq = g_state.seq.load(std::memory_order_relaxed);
            g_state.seq.store(seq + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);  // odd before any clock is read
#if defined(__aarch64__) || defined(_M_ARM64)
            // A fence orders memory accesses only: mrs cntvct_el0 could read
            // the counter before the odd count was visible, and a reader still
            // passing its check then got a later mftb than the frozen one (up
            // to 3.8 us back, tests/guest_time_test.cpp). dsb waits for the
            // store, isb keeps the counter read after it.
            asm volatile("dsb ish\n\tisb" ::: "memory");
#endif
            change();
            g_state.seq.store(seq + 2, std::memory_order_release);
        }

        template<class T>
        T ReadConsistent(T (*get)())
        {
            while (true)
            {
                uint64_t seq = g_state.seq.load(std::memory_order_acquire);
                if (seq & 1)
                    continue;
                T v = get();
                std::atomic_thread_fence(std::memory_order_acquire);
                if (g_state.seq.load(std::memory_order_relaxed) == seq)
                    return v;
            }
        }

        void HookThread()
        {
            std::unique_lock lock(s_mutex);
            while (true)
            {
                s_hookCv.wait(lock, [] { return s_hooked != s_resumes; });
                s_hooked = s_resumes;
                void (*hook)() = s_hook;
                lock.unlock();
                if (hook)
                    hook();
                lock.lock();
            }
        }
    }

    bool Suspended()
    {
        return ReadConsistent<bool>([] { return g_state.suspended.load(std::memory_order_relaxed); });
    }

    int64_t PausedNs()
    {
        return ReadConsistent<int64_t>([] { return g_state.ns.offset.load(std::memory_order_relaxed); });
    }

    uint64_t Epoch()
    {
        return s_epoch.load(std::memory_order_acquire);
    }

    int64_t LastResumeNs()
    {
        return s_lastResume.load(std::memory_order_acquire);
    }

    bool Suspend()
    {
        std::lock_guard lock(s_mutex);
        if (g_state.suspended.load(std::memory_order_relaxed))
            return false;
        // The epoch first: a thread that parks anywhere in this suspension
        // sees it changed once it's back, whichever it checks.
        s_epoch.fetch_add(1, std::memory_order_acq_rel);
        Write([] {
            auto freeze = [](Domain& d, int64_t raw, int64_t margin) {
                d.frozen.store(raw - d.offset.load(std::memory_order_relaxed) + margin, std::memory_order_relaxed);
            };
            freeze(g_state.timebase, RawTimebase(), kMarginTicks);
            freeze(g_state.ns, RawNs(), kMarginNs);
            freeze(g_state.system, RawSystem(), kMargin100ns);
            g_state.suspended.store(true, std::memory_order_release);
        });
        // The hook's thread, from the first suspension: a run that never
        // suspends (every desktop run) has the threads it always had.
        // Creating it waits on nothing. It finds s_resumes ahead of s_hooked
        // even if the Resume comes first.
        static std::once_flag started;
        std::call_once(started, [] { std::thread(HookThread).detach(); });
        return true;
    }

    bool Resume()
    {
        {
            std::lock_guard lock(s_mutex);
            if (!g_state.suspended.load(std::memory_order_relaxed))
                return false;
            Write([] {
                auto thaw = [](Domain& d, int64_t raw, int64_t margin) {
                    d.offset.store(raw - d.frozen.load(std::memory_order_relaxed) - margin, std::memory_order_relaxed);
                };
                thaw(g_state.timebase, RawTimebase(), kMarginTicks);
                thaw(g_state.ns, RawNs(), kMarginNs);
                thaw(g_state.system, RawSystem(), kMargin100ns);
                g_state.suspended.store(false, std::memory_order_release);
            });
            s_lastResume.store(RawNs(), std::memory_order_release);
            s_resumes++;
        }
        s_cv.notify_all();
        s_hookCv.notify_all();
        return true;
    }

    void WaitWhileSuspendedSlow()
    {
        std::unique_lock lock(s_mutex);
        s_cv.wait(lock, [] { return !g_state.suspended.load(std::memory_order_relaxed); });
    }

    void SleepUntil(int64_t deadline)
    {
        while (true)
        {
            WaitWhileSuspended();
            int64_t left = deadline - NowNs();
            if (left <= 0)
                return;
            std::this_thread::sleep_for(std::chrono::nanoseconds(left));
        }
    }

    void SetResumeHook(void (*hook)())
    {
        std::lock_guard lock(s_mutex);
        s_hook = hook;
    }
}
