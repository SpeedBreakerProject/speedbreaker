// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The game's clocks, which stand still while the game is suspended (iOS: the
// app isn't the active one, from Control Center to the background). Every
// time the guest can observe comes from here or waits on it: the timebase
// (mftb, its QueryPerformanceCounter), the system time, the tick count, wait
// timeouts, kernel timers, Sleep, the vblank interrupt and the audio render
// cadence. Guest time is host time minus the time spent suspended, so after
// a suspension every clock carries on from where it stopped and nothing
// catches up (on 2026-10-02 the intro movie ran at 2x after Control Center:
// only the GPU had stopped).
//
// Nothing suspends on desktop (SDL sends the app events only on iOS and
// Android; NFSMW_TEST_SUSPEND does in tests): the offsets stay 0 and every
// reading is the host's, as before.
//
// Force-included into the recompiled code ahead of ppc_context.h, whose
// PPC_QUERY_TIMEBASE this replaces, so it uses standard headers only.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace guesttime
{
    // A guest clock: guest = raw - offset, or frozen while suspended.
    struct Domain
    {
        std::atomic<int64_t> offset{ 0 };
        std::atomic<int64_t> frozen{ 0 };
    };

    // One seqlock over every domain, so a reader never mixes a clock's old
    // offset with the new suspended flag.
    struct State
    {
        std::atomic<uint64_t> seq{ 0 };  // odd while Suspend/Resume rewrite the rest
        std::atomic<bool> suspended{ false };
        Domain timebase;  // 49.875 MHz ticks (mftb)
        Domain ns;        // steady_clock ns: timeouts, timers, Sleep, the tick count, vblank, audio
        Domain system;    // 100 ns units since 1601 (KeQuerySystemTime)
    };
    extern State g_state;

    // The 360's timebase rate (what KeQueryPerformanceFrequency reports).
    inline constexpr uint64_t kTimebaseHz = 49875000;

    // The host clocks as the runtime read them before (ppc_context.h's
    // PpcQueryTimebase for the timebase).
    inline int64_t RawTimebase()
    {
#if defined(__aarch64__) || defined(_M_ARM64)
        uint64_t ticks, frequency;
        asm volatile("mrs %0, cntvct_el0" : "=r"(ticks)::"memory");
        asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
        return int64_t((unsigned __int128)ticks * kTimebaseHz / frequency);
#else
        uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        return int64_t((unsigned __int128)ns * kTimebaseHz / 1000000000u);
#endif
    }

    inline int64_t RawNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    inline int64_t RawSystem()
    {
        constexpr int64_t kEpochDelta = 116444736000000000;  // 1601 to 1970 in 100 ns
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 100 +
            kEpochDelta;
    }

    template<int64_t (*Raw)(), Domain State::*D>
    inline int64_t Read()
    {
        while (true)
        {
            uint64_t seq = g_state.seq.load(std::memory_order_acquire);
            if (seq & 1)
                continue;  // a transition, a few us
            const Domain& d = g_state.*D;
            int64_t v = g_state.suspended.load(std::memory_order_relaxed) ? d.frozen.load(std::memory_order_relaxed)
                                                                           : Raw() - d.offset.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_state.seq.load(std::memory_order_relaxed) == seq)
                return v;
        }
    }

    // Guest time in each domain.
    inline int64_t Timebase() { return Read<RawTimebase, &State::timebase>(); }
    inline int64_t NowNs() { return Read<RawNs, &State::ns>(); }
    inline int64_t SystemTime100ns() { return Read<RawSystem, &State::system>(); }

    // Whether the game is suspended, and the ns spent suspended in all so far
    // (constant while suspended, it changes at Resume: grows, or after a
    // suspension under 2 us drops by the margins). Each reads through the
    // seqlock, so it agrees with the clocks at its own instant; two calls
    // can straddle a transition.
    bool Suspended();
    int64_t PausedNs();
    uint64_t Epoch();        // Suspend() calls that took effect so far
    int64_t LastResumeNs();  // host steady ns of the last Resume (0: none yet)

    // Any thread holding no lock the main thread could need: returns once the
    // game isn't suspended.
    void WaitWhileSuspendedSlow();
    inline void WaitWhileSuspended()
    {
        if (g_state.suspended.load(std::memory_order_acquire))
            WaitWhileSuspendedSlow();
    }
    // Sleeps until guest time `deadline` (ns), longer by any suspension.
    void SleepUntil(int64_t deadline);

    // The main thread (iOS: inside UIKit's scene callbacks), idempotent:
    // UIKit reports each transition twice. True when it changed something.
    // Neither waits on a game thread nor takes a game lock: Resume wakes
    // the kernel's waiters from a thread of its own (SetResumeHook).
    bool Suspend();
    bool Resume();
    // Called after every Resume, on a helper thread (never Resume's caller),
    // to wake waits parked without a timeout while suspended.
    void SetResumeHook(void (*hook)());
}

// The recompiled code's mftb (runtime files that include ppc_context.h
// first keep its default, which they don't use).
inline uint64_t GuestTimebase()
{
    return uint64_t(guesttime::Timebase());
}
#ifndef PPC_QUERY_TIMEBASE
#define PPC_QUERY_TIMEBASE() GuestTimebase()
#endif
